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
#ifndef SYSMON_H
#define SYSMON_H

#define MAX_CORES 64           /* upper limit of logical CPUs we track */
#define MAX_IFACES 8           /* upper limit of network interfaces we track */
#define MAX_DISKS 8            /* upper limit of mounted filesystems we show */
#define MAX_DISKIO 8           /* upper limit of physical disks we track I/O for */
#define SAMPLE_INTERVAL_SEC 1  /* seconds between samples; rates are divided by this */

struct NetIface {
    char name[16];             /* e.g. "eth0" (the kernel's limit is 15 chars + '\0') */
    unsigned long prev_rx;     /* byte counters from the previous sample */
    unsigned long prev_tx;
    float rx_rate;             /* download, bytes per second */
    float tx_rate;             /* upload, bytes per second */
    int seen;                  /* set each tick; interfaces that vanish get dropped */
};

struct DiskUsage {
    char mount[64];            /* mount point, e.g. "/" or "/home" */
    char fstype[16];           /* e.g. "ext4" */
    float total_gb;
    float used_gb;
    float used_pct;
};

struct DiskIO {
    char name[32];             /* e.g. "nvme0n1" or "sda" */
    unsigned long prev_read;   /* sector counters from the previous sample */
    unsigned long prev_write;
    float read_rate;           /* bytes per second */
    float write_rate;
    int seen;
};

struct SystemStatus {
    int cpu_temp;
    int cpu_temp_valid;        /* 1 if a CPU temperature sensor was found and read */
    unsigned long prev_idle;
    unsigned long prev_total;
    float cpu_usage_pct;       /* overall CPU usage */
    unsigned long cpu_total_ticks;   /* all CPU time since boot, summed over every core (for per-process %) */

    int num_cores;                          /* how many cores were read this tick */
    int core_cpu[MAX_CORES];                /* the kernel's CPU number of each slot (cpu0, cpu1, ...) */
    float core_usage_pct[MAX_CORES];        /* usage of each core */
    int core_temp[MAX_CORES];               /* temperature of each core's physical core in C, or -1 */
    int core_temps_valid;                   /* 1 if at least one per-core temperature was read */
    unsigned long core_prev_idle[MAX_CORES];   /* previous sample, per core */
    unsigned long core_prev_total[MAX_CORES];

    int num_ifaces;
    struct NetIface ifaces[MAX_IFACES];

    int num_disks;
    struct DiskUsage disks[MAX_DISKS];

    int num_diskio;
    struct DiskIO diskio[MAX_DISKIO];

    int uptime_seconds;
    float ram_total_gb;
    float ram_free_gb;
    float ram_avail_gb;
    int total_processes;
    char status_message[100];
};

void get_system_status(struct SystemStatus *status);

#endif
