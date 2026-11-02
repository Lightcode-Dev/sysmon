/*
 * sysmon - live terminal system monitor
 * Copyright (C) 2026 Nathan Burke
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/\>.
 */
#define _POSIX_C_SOURCE 200809L   /* must come before any #include: enables statvfs/snprintf/access under -ansi */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include "sysmon.h"


#define MAX_THERMAL_ZONES 32

/* Where the kernel exposes sensors and CPU topology (overridable at build time, for testing) */
#ifndef HWMON_DIR
#define HWMON_DIR "/sys/class/hwmon"
#endif
#ifndef CPU_SYS_DIR
#define CPU_SYS_DIR "/sys/devices/system/cpu"
#endif

/* Thermal zone "type" names that mean "the CPU", in priority order */
static const char *cpu_zone_types[] = {
    "x86_pkg_temp",  /* Intel */
    "k10temp",       /* AMD */
    "coretemp",
    "cpu-thermal"    /* ARM boards */
};
#define NUM_CPU_ZONE_TYPES (sizeof(cpu_zone_types) / sizeof(cpu_zone_types[0]))

/* Find the temp file of the CPU thermal zone. Zone numbers are not stable
   between machines or kernels, so we read each zone's "type" instead.
   The result is cached: we only search once. */
static const char *find_cpu_temp_path(void) {
    static char temp_path[80];
    static int searched = 0;
    char type_path[80];
    char type[64];
    FILE *f;
    unsigned int t;
    int zone;

    if (searched) {
        return temp_path[0] != '\0' ? temp_path : NULL;
    }
    searched = 1;

    /* Outer loop is priority, so x86_pkg_temp wins over a generic match */
    for (t = 0; t < NUM_CPU_ZONE_TYPES; t++) {
        for (zone = 0; zone < MAX_THERMAL_ZONES; zone++) {
            sprintf(type_path, "/sys/class/thermal/thermal_zone%d/type", zone);
            f = fopen(type_path, "r");
            if (f == NULL) continue;

            if (fgets(type, sizeof(type), f) != NULL) {
                /* strncmp with the pattern length ignores the trailing newline */
                if (strncmp(type, cpu_zone_types[t], strlen(cpu_zone_types[t])) == 0) {
                    fclose(f);
                    sprintf(temp_path, "/sys/class/thermal/thermal_zone%d/temp", zone);
                    return temp_path;
                }
            }
            fclose(f);
        }
    }
    return NULL;
}

/* Turn the 8 raw /proc/stat counters into "idle" and "total" tick counts.
   Order of t[]: user nice system idle iowait irq softirq steal */
static void times_to_totals(const unsigned long t[8], unsigned long *idle_all,
                            unsigned long *total) {
    *idle_all = t[3] + t[4];                                  /* idle + iowait */
    *total = *idle_all + t[0] + t[1] + t[2] + t[5] + t[6] + t[7];
}

/* Usage % between the previous sample and this one. Updates the stored
   previous values. If no time has passed, keeps the old percentage. */
static float cpu_percent(unsigned long idle, unsigned long total,
                         unsigned long *prev_idle, unsigned long *prev_total,
                         float old_pct) {
    unsigned long total_diff, idle_diff;
    float pct = old_pct;

    total_diff = total - *prev_total;
    idle_diff = idle - *prev_idle;
    if (total_diff != 0) {
        pct = (float)(total_diff - idle_diff) / (float)total_diff * 100.0f;
    }
    *prev_total = total;
    *prev_idle = idle;
    return pct;
}

/* Copy src into dst, never writing more than dst_size bytes, always ending in '\0' */
static void copy_str(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Space used on each mounted block-device filesystem (what "df" shows) */
static void read_disk_space(struct SystemStatus *status) {
    FILE *f;
    char line[512];
    char dev[128], mnt[128], fstype[32];
    char seen_dev[MAX_DISKS][128];   /* devices already listed (skips duplicate mounts) */
    struct statvfs vfs;
    double total, used, avail;
    int i, dup;

    status->num_disks = 0;
    f = fopen("/proc/mounts", "r");
    if (f == NULL) return;

    /* Each line: "<device> <mount point> <fstype> <options> 0 0" */
    while (status->num_disks < MAX_DISKS && fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%127s %127s %31s", dev, mnt, fstype) != 3) continue;

        /* Only real block devices. This drops tmpfs, proc, overlay and so on,
           and loop devices (snap/appimage mounts). */
        if (strncmp(dev, "/dev/", 5) != 0) continue;
        if (strncmp(dev, "/dev/loop", 9) == 0) continue;
        if (strcmp(fstype, "squashfs") == 0) continue;   /* read-only images, always 100% */

        /* Btrfs subvolumes and bind mounts list one device many times: show it once */
        dup = 0;
        for (i = 0; i < status->num_disks; i++) {
            if (strcmp(seen_dev[i], dev) == 0) { dup = 1; break; }
        }
        if (dup) continue;

        if (statvfs(mnt, &vfs) != 0 || vfs.f_blocks == 0) continue;

        /* Same maths as df: used = blocks - free, percent of (used + available to us) */
        total = (double)vfs.f_blocks * (double)vfs.f_frsize;
        used = (double)(vfs.f_blocks - vfs.f_bfree) * (double)vfs.f_frsize;
        avail = (double)vfs.f_bavail * (double)vfs.f_frsize;

        i = status->num_disks;
        strcpy(seen_dev[i], dev);
        copy_str(status->disks[i].mount, sizeof(status->disks[i].mount), mnt);
        copy_str(status->disks[i].fstype, sizeof(status->disks[i].fstype), fstype);
        status->disks[i].total_gb = (float)(total / 1073741824.0);
        status->disks[i].used_gb = (float)(used / 1073741824.0);
        status->disks[i].used_pct = (used + avail > 0.0)
            ? (float)(used / (used + avail) * 100.0) : 0.0f;
        status->num_disks++;
    }
    fclose(f);
}

/* Read/write speed of each physical disk, from /proc/diskstats.
   Line: "<major> <minor> <name> <reads> <merged> <sectors read> <ms> <writes>
          <merged> <sectors written> ..." Sectors are always 512 bytes. */
static void read_disk_io(struct SystemStatus *status) {
    FILE *f;
    char line[256], name[32], path[64];
    unsigned long rd, wr;
    int i, j;

    f = fopen("/proc/diskstats", "r");
    if (f == NULL) return;

    for (i = 0; i < status->num_diskio; i++) {
        status->diskio[i].seen = 0;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        /* %*s skips a field. Skip major, minor; read name; skip 2; read sectors
           read; skip 3; read sectors written. */
        if (sscanf(line, "%*s %*s %31s %*s %*s %lu %*s %*s %*s %lu",
                   name, &rd, &wr) != 3) continue;

        /* Virtual devices that are not real hardware */
        if (strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 ||
            strncmp(name, "zram", 4) == 0) continue;

        /* Whole disks have a directory in /sys/block; partitions (sda1,
           nvme0n1p1) do not. That is a reliable way to tell them apart. */
        sprintf(path, "/sys/block/%s", name);
        if (access(path, F_OK) != 0) continue;

        for (i = 0; i < status->num_diskio; i++) {
            if (strcmp(status->diskio[i].name, name) == 0) break;
        }
        if (i == status->num_diskio) {
            if (status->num_diskio >= MAX_DISKIO) continue;
            strcpy(status->diskio[i].name, name);   /* fits: name is at most 31 chars */
            status->diskio[i].read_rate = 0.0f;
            status->diskio[i].write_rate = 0.0f;
            status->num_diskio++;
        } else {
            status->diskio[i].read_rate = (rd >= status->diskio[i].prev_read)
                ? (float)(rd - status->diskio[i].prev_read) * 512.0f / SAMPLE_INTERVAL_SEC : 0.0f;
            status->diskio[i].write_rate = (wr >= status->diskio[i].prev_write)
                ? (float)(wr - status->diskio[i].prev_write) * 512.0f / SAMPLE_INTERVAL_SEC : 0.0f;
        }
        status->diskio[i].prev_read = rd;
        status->diskio[i].prev_write = wr;
        status->diskio[i].seen = 1;
    }
    fclose(f);

    /* Drop disks that disappeared (e.g. an unplugged USB drive) */
    j = 0;
    for (i = 0; i < status->num_diskio; i++) {
        if (status->diskio[i].seen) {
            if (j != i) status->diskio[j] = status->diskio[i];
            j++;
        }
    }
    status->num_diskio = j;
}

/* Per-core temperatures from the Intel "coretemp" driver. It lists one sensor per
   PHYSICAL core (labelled "Core 0", "Core 1", ...), while /proc/stat lists
   LOGICAL CPUs, so with hyperthreading two logical CPUs share one sensor. Each
   CPU's topology/core_id file says which physical core it belongs to.
   AMD's k10temp has only a package sensor, so there per-core temps stay hidden. */
static void read_core_temps(struct SystemStatus *status) {
    static char sensor_dir[96];           /* e.g. /sys/class/hwmon/hwmon3, cached */
    static int searched = 0;
    static int topo_core[MAX_CORES];      /* cpu number -> physical core id (-1 unknown) */
    static int topo_read[MAX_CORES];
    int phys_temp[MAX_CORES];
    char path[160], label[32];
    FILE *f;
    int i, t, core, milli, cpu;

    status->core_temps_valid = 0;
    for (i = 0; i < status->num_cores; i++) status->core_temp[i] = -1;

    if (!searched) {
        searched = 1;
        for (i = 0; i < 32; i++) {
            sprintf(path, HWMON_DIR "/hwmon%d/name", i);
            f = fopen(path, "r");
            if (f == NULL) continue;
            if (fgets(label, sizeof(label), f) != NULL && strncmp(label, "coretemp", 8) == 0) {
                sprintf(sensor_dir, HWMON_DIR "/hwmon%d", i);
                fclose(f);
                break;
            }
            fclose(f);
        }
    }
    if (sensor_dir[0] == '\0') return;     /* no coretemp driver */

    for (i = 0; i < MAX_CORES; i++) phys_temp[i] = -1;
    for (t = 1; t < 64; t++) {             /* temp1_label, temp2_label, ... until one is missing */
        sprintf(path, "%s/temp%d_label", sensor_dir, t);
        f = fopen(path, "r");
        if (f == NULL) break;
        core = -1;
        if (fgets(label, sizeof(label), f) == NULL || sscanf(label, "Core %d", &core) != 1) core = -1;
        fclose(f);
        if (core < 0 || core >= MAX_CORES) continue;     /* skip the "Package id 0" sensor */

        sprintf(path, "%s/temp%d_input", sensor_dir, t);
        f = fopen(path, "r");
        if (f == NULL) continue;
        if (fscanf(f, "%d", &milli) == 1) phys_temp[core] = milli / 1000;
        fclose(f);
    }

    for (i = 0; i < status->num_cores; i++) {
        cpu = status->core_cpu[i];
        if (cpu < 0 || cpu >= MAX_CORES) continue;
        if (!topo_read[cpu]) {
            topo_read[cpu] = 1;
            topo_core[cpu] = -1;
            sprintf(path, CPU_SYS_DIR "/cpu%d/topology/core_id", cpu);
            f = fopen(path, "r");
            if (f != NULL) {
                if (fscanf(f, "%d", &core) == 1) topo_core[cpu] = core;
                fclose(f);
            }
        }
        core = topo_core[cpu];
        if (core >= 0 && core < MAX_CORES && phys_temp[core] >= 0) {
            status->core_temp[i] = phys_temp[core];
            status->core_temps_valid = 1;
        }
    }
}

void get_system_status(struct SystemStatus *status) {
    FILE *temp_file, *uptime_file, *mem_file, *load_file, *stat_file, *net_file;
    const char *temp_path;
    int millidegrees, mem_total_kb = 0, mem_free_kb = 0, mem_avail_kb = 0;
    float raw_uptime;
    char buffer[256];

    unsigned long t[8];
    unsigned long idle_all, total;
    int cores, cpu_id;
    char line[512], name[16];
    char *p, *colon;
    unsigned long rx, tx;
    int len, i, j;

    /* 1. CPU Usage: the "cpu " line is the overall total, then one
          "cpuN" line per core. They are all at the top of /proc/stat. */
    stat_file = fopen("/proc/stat", "r");
    if (stat_file != NULL) {
        cores = 0;
        while (fgets(buffer, sizeof(buffer), stat_file) != NULL) {
            if (strncmp(buffer, "cpu", 3) != 0) break;   /* past the cpu lines */

            if (buffer[3] == ' ') {
                /* Overall line: "cpu  ..." (note the space after "cpu") */
                if (sscanf(buffer, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                           &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 8) {
                    times_to_totals(t, &idle_all, &total);
                    status->cpu_total_ticks = total;
                    status->cpu_usage_pct = cpu_percent(idle_all, total,
                            &status->prev_idle, &status->prev_total,
                            status->cpu_usage_pct);
                }
            } else if (cores < MAX_CORES) {
                /* Core line: "cpu0 ...". %d reads the CPU number, kept for the temperature lookup */
                if (sscanf(buffer, "cpu%d %lu %lu %lu %lu %lu %lu %lu %lu", &cpu_id,
                           &t[0], &t[1], &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 9) {
                    status->core_cpu[cores] = cpu_id;
                    times_to_totals(t, &idle_all, &total);
                    status->core_usage_pct[cores] = cpu_percent(idle_all, total,
                            &status->core_prev_idle[cores],
                            &status->core_prev_total[cores],
                            status->core_usage_pct[cores]);
                    cores++;
                }
            }
        }
        status->num_cores = cores;
        fclose(stat_file);
    }

    /* 2. CPU Temp */
    status->cpu_temp_valid = 0;
    temp_path = find_cpu_temp_path();
    if (temp_path != NULL) {
        temp_file = fopen(temp_path, "r");
        if (temp_file != NULL) {
            if (fscanf(temp_file, "%d", &millidegrees) == 1) {
                status->cpu_temp = millidegrees / 1000;
                status->cpu_temp_valid = 1;
            }
            fclose(temp_file);
        }
    }

    /* 3. Uptime */
    uptime_file = fopen("/proc/uptime", "r");
    if (uptime_file != NULL) {
        if (fscanf(uptime_file, "%f", &raw_uptime) == 1) {
            status->uptime_seconds = (int)raw_uptime;
        }
        fclose(uptime_file);
    }

    /* 4. Memory */
    mem_file = fopen("/proc/meminfo", "r");
    if (mem_file != NULL) {
        while (fgets(buffer, sizeof(buffer), mem_file) != NULL) {
            if (strncmp(buffer, "MemTotal:", 9) == 0) {
                sscanf(buffer, "MemTotal: %d kB", &mem_total_kb);
            }
            if (strncmp(buffer, "MemFree:", 8) == 0) {
                sscanf(buffer, "MemFree: %d kB", &mem_free_kb);
            }
            if (strncmp(buffer, "MemAvailable:", 13) == 0) {
                sscanf(buffer, "MemAvailable: %d kB", &mem_avail_kb);
            }
        }
        status->ram_total_gb = mem_total_kb / (1024.0 * 1024.0);
        status->ram_free_gb = mem_free_kb / (1024.0 * 1024.0);
        status->ram_avail_gb = mem_avail_kb / (1024.0 * 1024.0);
        fclose(mem_file);
    }

    /* 5. Processes */
    load_file = fopen("/proc/loadavg", "r");
    if (load_file != NULL) {
        /* %*f and %*d/ skip fields without storing them; the return is 1 if the one stored field parsed */
        if (fscanf(load_file, "%*f %*f %*f %*d/%d", &status->total_processes) != 1) {
            status->total_processes = 0;
        }
        fclose(load_file);
    }

    /* 6. Network: /proc/net/dev has 2 header lines, then one line per interface:
          "  eth0: <rx bytes> <7 more rx fields> <tx bytes> <7 more tx fields>" */
    net_file = fopen("/proc/net/dev", "r");
    if (net_file != NULL) {
        for (i = 0; i < status->num_ifaces; i++) {
            status->ifaces[i].seen = 0;   /* re-marked below if still present */
        }

        /* Skip the two header lines */
        if (fgets(line, sizeof(line), net_file) != NULL &&
            fgets(line, sizeof(line), net_file) != NULL) {
            while (fgets(line, sizeof(line), net_file) != NULL) {
                p = line;
                while (*p == ' ') p++;            /* names are right-aligned */
                colon = strchr(p, ':');
                if (colon == NULL) continue;
                len = (int)(colon - p);
                if (len <= 0 || len >= (int)sizeof(name)) continue;
                memcpy(name, p, len);
                name[len] = '\0';
                if (strcmp(name, "lo") == 0) continue;   /* skip loopback */

                /* Parse after the colon (it can touch the first number).
                   %*s skips one field: take rx bytes, skip 7, take tx bytes. */
                if (sscanf(colon + 1, "%lu %*s %*s %*s %*s %*s %*s %*s %lu",
                           &rx, &tx) != 2) continue;

                /* Look this interface up by name */
                for (i = 0; i < status->num_ifaces; i++) {
                    if (strcmp(status->ifaces[i].name, name) == 0) break;
                }
                if (i == status->num_ifaces) {
                    /* Not seen before: add it, with a 0 rate for the first sample */
                    if (status->num_ifaces >= MAX_IFACES) continue;
                    strcpy(status->ifaces[i].name, name);   /* fits: len < 16 */
                    status->ifaces[i].rx_rate = 0.0f;
                    status->ifaces[i].tx_rate = 0.0f;
                    status->num_ifaces++;
                } else {
                    /* A counter that went down means the interface was reset: use 0 */
                    status->ifaces[i].rx_rate = (rx >= status->ifaces[i].prev_rx)
                        ? (float)(rx - status->ifaces[i].prev_rx) / SAMPLE_INTERVAL_SEC : 0.0f;
                    status->ifaces[i].tx_rate = (tx >= status->ifaces[i].prev_tx)
                        ? (float)(tx - status->ifaces[i].prev_tx) / SAMPLE_INTERVAL_SEC : 0.0f;
                }
                status->ifaces[i].prev_rx = rx;
                status->ifaces[i].prev_tx = tx;
                status->ifaces[i].seen = 1;
            }
        }
        fclose(net_file);

        /* Drop interfaces that disappeared (e.g. a removed docker veth) */
        j = 0;
        for (i = 0; i < status->num_ifaces; i++) {
            if (status->ifaces[i].seen) {
                if (j != i) status->ifaces[j] = status->ifaces[i];
                j++;
            }
        }
        status->num_ifaces = j;
    }

    /* 6b. Per-core temperatures (needs the core list from step 1) */
    read_core_temps(status);

    /* 7. Disks */
    read_disk_space(status);
    read_disk_io(status);

    /* strncpy does not always add the '\0', so we add it ourselves */
    strncpy(status->status_message, "Live kernel data refreshing...",
            sizeof(status->status_message) - 1);
    status->status_message[sizeof(status->status_message) - 1] = '\0';
}
