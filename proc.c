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
#define _POSIX_C_SOURCE 200809L   /* before any #include: enables opendir, getpwuid, sysconf, snprintf */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "sysmon.h"
#include "proc.h"

/* Copy src into dst, never writing more than dst_size bytes, always ending in '\0' */
static void copy_str(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* Process names come from other programs. A name containing a raw ESC byte
   could inject terminal escape sequences into our display, so every control
   character becomes a space. */
static void sanitize(char *s) {
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 32 || *s == 127) *s = ' ';
    }
}

/* uid -> user name, cached because the lookup can be slow (it may consult
   /etc/passwd, LDAP, ...) and the same few users own most processes */
static void lookup_user(uid_t uid, char *out, size_t out_size) {
    static struct { uid_t uid; char name[16]; } cache[64];
    static int cache_n = 0;
    struct passwd *pw;
    char tmp[16];
    int i;

    for (i = 0; i < cache_n; i++) {
        if (cache[i].uid == uid) {
            copy_str(out, out_size, cache[i].name);
            return;
        }
    }
    pw = getpwuid(uid);
    if (pw != NULL) {
        copy_str(tmp, sizeof(tmp), pw->pw_name);
    } else {
        sprintf(tmp, "%lu", (unsigned long)uid);   /* no name known: show the number */
    }
    if (cache_n < 64) {
        cache[cache_n].uid = uid;
        copy_str(cache[cache_n].name, sizeof(cache[cache_n].name), tmp);
        cache_n++;
    }
    copy_str(out, out_size, tmp);
}

/* Fill p->cmd from /proc/<pid>/cmdline (arguments separated by NUL bytes) */
static void read_cmdline(struct ProcInfo *p) {
    char path[48];
    char buf[sizeof(p->cmd)];
    FILE *f;
    size_t n, i;

    sprintf(path, "/proc/%d/cmdline", p->pid);
    n = 0;
    f = fopen(path, "r");
    if (f != NULL) {
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[n] = '\0';

    /* Turn the NUL separators into spaces, then trim trailing ones */
    for (i = 0; i + 1 < n; i++) {
        if (buf[i] == '\0') buf[i] = ' ';
    }
    while (n > 0 && (buf[n - 1] == '\0' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    sanitize(buf);

    if (n == 0) {
        /* Kernel threads have no command line; show [name] like ps does */
        sprintf(p->cmd, "[%s]", p->comm);
    } else {
        copy_str(p->cmd, sizeof(p->cmd), buf);
    }
}

/* Parse /proc/<pid>/stat into p. Returns 0 if the process vanished or was unreadable. */
static int read_proc_stat(int pid, struct ProcInfo *p) {
    char path[48], line[1024];
    char *lp, *rp;
    FILE *f;
    struct stat sb;
    unsigned long utime, stime, starttime;
    long nice, threads, rss;
    char state;
    int len, ppid;

    memset(p, 0, sizeof(*p));
    p->pid = pid;

    sprintf(path, "/proc/%d/stat", pid);
    f = fopen(path, "r");
    if (f == NULL) return 0;
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);

    /* Line looks like: 1234 (name) S 1 ... . The name is in parentheses and
       may itself contain spaces or ')' , so split on the FIRST '(' and the LAST ')'. */
    lp = strchr(line, '(');
    rp = strrchr(line, ')');
    if (lp == NULL || rp == NULL || rp < lp) return 0;
    len = (int)(rp - lp) - 1;
    if (len > (int)sizeof(p->comm) - 1) len = (int)sizeof(p->comm) - 1;
    memcpy(p->comm, lp + 1, len);
    p->comm[len] = '\0';
    sanitize(p->comm);

    /* After ") " the fields are numbered (see man 5 proc). Field 3 is the state;
       %*s skips a field. We want 4 ppid, 14 utime, 15 stime, 19 nice,
       20 num_threads, 22 starttime and 24 rss (in pages). */
    if (sscanf(rp + 2,
               "%c %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu %*s %*s %*s %ld %ld %*s %lu %*s %ld",
               &state, &ppid, &utime, &stime, &nice, &threads, &starttime, &rss) != 8) {
        return 0;
    }
    p->state = state;
    p->ppid = ppid;
    p->ticks = utime + stime;
    p->nice = (int)nice;
    p->threads = (int)threads;
    p->starttime = starttime;
    if (rss < 0) rss = 0;
    p->rss_kb = (unsigned long)rss * (unsigned long)(sysconf(_SC_PAGESIZE) / 1024);

    /* The owner of the /proc/<pid> directory is the owner of the process */
    sprintf(path, "/proc/%d", pid);
    if (stat(path, &sb) == 0) {
        lookup_user(sb.st_uid, p->user, sizeof(p->user));
    } else {
        copy_str(p->user, sizeof(p->user), "?");
    }
    return 1;
}

/* Fill the I/O counters from /proc/<pid>/io. Lines look like "read_bytes: 4096".
   These count real storage traffic (page-cache hits do not count). Reading another
   user's file is not allowed, so for them io_ok stays 0. */
static void read_proc_io(int pid, struct ProcInfo *p) {
    char path[48], line[96];
    FILE *f;
    int found;

    sprintf(path, "/proc/%d/io", pid);
    f = fopen(path, "r");
    if (f == NULL) return;
    found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "read_bytes:", 11) == 0 && sscanf(line + 11, "%lu", &p->io_read) == 1) found |= 1;
        else if (strncmp(line, "write_bytes:", 12) == 0 && sscanf(line + 12, "%lu", &p->io_write) == 1) found |= 2;
    }
    fclose(f);
    p->io_ok = (found == 3);
}

static int all_digits(const char *s) {
    if (*s == '\0') return 0;
    for (; *s != '\0'; s++) {
        if (!isdigit((unsigned char)*s)) return 0;
    }
    return 1;
}

static int cmp_pid(const void *a, const void *b) {
    const struct ProcInfo *x = (const struct ProcInfo *)a;
    const struct ProcInfo *y = (const struct ProcInfo *)b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

void proc_update(struct ProcList *pl, int num_cores, unsigned long total_ticks, float ram_total_gb,
                 float dt) {
    DIR *d;
    struct dirent *e;
    struct ProcInfo *old, *cur;
    int n, i, j, old_n;
    unsigned long total_diff;
    float ram_total_kb;

    old = pl->buf[pl->cur];
    cur = pl->buf[pl->cur ^ 1];
    old_n = pl->count;
    if (num_cores < 1) num_cores = 1;

    /* Pass 1: read every numeric directory in /proc (each one is a pid) */
    d = opendir("/proc");
    if (d == NULL) return;
    n = 0;
    while (n < MAX_PROCS && (e = readdir(d)) != NULL) {
        if (!all_digits(e->d_name)) continue;
        if (read_proc_stat(atoi(e->d_name), &cur[n])) {
            read_proc_io(cur[n].pid, &cur[n]);
            n++;
        }
    }
    closedir(d);
    qsort(cur, n, sizeof(cur[0]), cmp_pid);   /* both lists are now sorted by pid */

    /* Pass 2: walk the two sorted lists together. For a process present in both
       we can compute CPU% from the change in ticks, and we reuse its command line
       instead of re-reading it every second. */
    total_diff = total_ticks - pl->prev_total_ticks;
    ram_total_kb = ram_total_gb * 1024.0f * 1024.0f;
    j = 0;
    for (i = 0; i < n; i++) {
        while (j < old_n && old[j].pid < cur[i].pid) j++;

        if (j < old_n && old[j].pid == cur[i].pid && old[j].starttime == cur[i].starttime) {
            cur[i].prev_ticks = old[j].ticks;
            if (cur[i].ticks >= cur[i].prev_ticks && total_diff > 0) {
                /* total_diff is summed over all cores, so total_diff / num_cores is
                   the ticks ONE core could have used in this interval */
                cur[i].cpu_pct = (float)(cur[i].ticks - cur[i].prev_ticks) * 100.0f
                                 * (float)num_cores / (float)total_diff;
            }
            if (cur[i].io_ok && old[j].io_ok) {
                if (cur[i].io_read >= old[j].io_read) {
                    cur[i].io_read_rate = (float)(cur[i].io_read - old[j].io_read) / dt;
                }
                if (cur[i].io_write >= old[j].io_write) {
                    cur[i].io_write_rate = (float)(cur[i].io_write - old[j].io_write) / dt;
                }
                cur[i].io_rate = cur[i].io_read_rate + cur[i].io_write_rate;
            }
            if (strcmp(old[j].comm, cur[i].comm) == 0) {
                memcpy(cur[i].cmd, old[j].cmd, sizeof(cur[i].cmd));
            } else {
                read_cmdline(&cur[i]);       /* the process ran exec() and changed program */
            }
        } else {
            /* New process (or a reused pid, which has a different starttime) */
            cur[i].prev_ticks = cur[i].ticks;
            read_cmdline(&cur[i]);
        }

        if (ram_total_kb > 0.0f) {
            cur[i].mem_pct = (float)cur[i].rss_kb / ram_total_kb * 100.0f;
        }
    }

    pl->cur ^= 1;                  /* the new sample becomes the current one */
    pl->count = n;
    pl->prev_total_ticks = total_ticks;
}

/* ---------- Sorting and filtering ---------- */

/* qsort cannot pass extra arguments to its comparison function in C90, so the
   current sort settings live in file-level variables. */
static int g_sort_key = SORT_CPU;
static int g_sort_desc = 1;

static int cmp_view(const void *a, const void *b) {
    const struct ProcInfo *x = *(struct ProcInfo * const *)a;
    const struct ProcInfo *y = *(struct ProcInfo * const *)b;
    int r = 0;

    switch (g_sort_key) {
    case SORT_CPU:  r = (x->cpu_pct > y->cpu_pct) - (x->cpu_pct < y->cpu_pct); break;
    case SORT_MEM:  r = (x->rss_kb > y->rss_kb) - (x->rss_kb < y->rss_kb); break;
    case SORT_PID:  r = (x->pid > y->pid) - (x->pid < y->pid); break;
    case SORT_USER: r = strcmp(x->user, y->user); break;
    case SORT_NAME: r = strcmp(x->cmd, y->cmd); break;
    case SORT_IO:   r = (x->io_rate > y->io_rate) - (x->io_rate < y->io_rate); break;
    }
    if (g_sort_desc) r = -r;
    if (r == 0) r = (x->pid > y->pid) - (x->pid < y->pid);   /* ties: lowest pid first */
    return r;
}

/* Does hay contain needle, ignoring case? needle must already be lowercase. */
static int contains_nocase(const char *hay, const char *needle) {
    int i, j;

    for (i = 0; hay[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0' && hay[i + j] != '\0'
                    && tolower((unsigned char)hay[i + j]) == needle[j]; j++) {
            /* just advancing j */
        }
        if (needle[j] == '\0') return 1;
    }
    return 0;
}

/* Does this process match the (lowercase) filter? An empty filter matches all. */
static int matches(const struct ProcInfo *p, const char *needle) {
    char pidtext[16];

    if (needle[0] == '\0') return 1;
    sprintf(pidtext, "%d", p->pid);
    return contains_nocase(p->cmd, needle) || contains_nocase(p->user, needle)
        || contains_nocase(pidtext, needle);
}

/* Branch pieces for tree view (UTF-8: box-drawing characters, each 3 columns wide) */
#define TREE_MID   "\xe2\x94\x9c\xe2\x94\x80 "    /* |-  */
#define TREE_LAST  "\xe2\x94\x94\xe2\x94\x80 "    /* `-  */
#define TREE_PIPE  "\xe2\x94\x82  "               /* |    */
#define TREE_BLANK "   "

static void append_str(char *dst, size_t size, const char *s) {
    size_t len = strlen(dst);
    if (len + strlen(s) < size) strcpy(dst + len, s);   /* silently skip if it would not fit */
}

/* Index of pid in list (sorted by pid), or -1. Binary search. */
static int find_pid_index(const struct ProcInfo *list, int n, int pid) {
    int lo = 0, hi = n - 1, mid;

    while (lo <= hi) {
        mid = (lo + hi) / 2;
        if (list[mid].pid == pid) return mid;
        if (list[mid].pid < pid) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Which branches are folded. Toggling an already-folded pid unfolds it. When the list
   is full the oldest entry is dropped (pids of dead processes would pile up otherwise). */
static int is_collapsed(const struct ProcList *pl, int pid) {
    int i;
    for (i = 0; i < pl->n_collapsed; i++) {
        if (pl->collapsed[i] == pid) return 1;
    }
    return 0;
}

void proc_toggle_collapse(struct ProcList *pl, int pid) {
    int i;

    for (i = 0; i < pl->n_collapsed; i++) {
        if (pl->collapsed[i] == pid) {
            for (; i + 1 < pl->n_collapsed; i++) pl->collapsed[i] = pl->collapsed[i + 1];
            pl->n_collapsed--;
            return;
        }
    }
    if (pl->n_collapsed == MAX_COLLAPSED) {
        for (i = 0; i + 1 < MAX_COLLAPSED; i++) pl->collapsed[i] = pl->collapsed[i + 1];
        pl->n_collapsed--;
    }
    pl->collapsed[pl->n_collapsed++] = pid;
}

/* Tree building state. children of process i form a linked list: child_head[i] is the
   first one and child_next[c] the next sibling (-1 ends the list). */
static int child_head[MAX_PROCS], child_next[MAX_PROCS];
static struct ProcInfo *sorted_ptrs[MAX_PROCS];
static struct ProcInfo *g_list;
static char g_needle[32];   /* a copy: the caller's buffer is gone once we return */
static struct ProcList *g_pl;
static int g_n;

/* How many processes are below idx in the tree */
static int count_descendants(int idx, int depth) {
    int c, n = 0;
    if (depth >= 64) return 0;
    for (c = child_head[idx]; c != -1; c = child_next[c]) {
        n += 1 + count_descendants(c, depth + 1);
    }
    return n;
}

/* Depth-first walk: emit a process, then all of its children. cont is the
   line-drawing prefix inherited from the ancestors. */
static void tree_walk(int idx, const char *cont, int is_last, int is_root, int depth) {
    struct ProcInfo *p = &g_list[idx];
    char child_cont[64];
    int c, next;

    p->tree[0] = '\0';
    child_cont[0] = '\0';
    if (!is_root) {
        copy_str(p->tree, sizeof(p->tree), cont);
        append_str(p->tree, sizeof(p->tree), is_last ? TREE_LAST : TREE_MID);
        copy_str(child_cont, sizeof(child_cont), cont);
        append_str(child_cont, sizeof(child_cont), is_last ? TREE_BLANK : TREE_PIPE);
    }
    if (g_n < MAX_PROCS && matches(p, g_needle)) {
        g_pl->view[g_n++] = p;
    }
    p->hidden = 0;
    /* A folded branch shows only its top process. With a filter active we ignore folding,
       otherwise matches inside a folded branch would be invisible. */
    if (g_needle[0] == '\0' && child_head[idx] != -1 && is_collapsed(g_pl, p->pid)) {
        p->hidden = count_descendants(idx, 0);
        return;
    }
    if (depth >= 64) return;       /* guard against absurdly deep chains */
    for (c = child_head[idx]; c != -1; c = next) {
        next = child_next[c];
        tree_walk(c, child_cont, next == -1, 0, depth + 1);
    }
}

void proc_build_view(struct ProcList *pl, int sort_key, int sort_desc, int tree, const char *filter) {
    struct ProcInfo *list;
    char needle[32];
    int i, n, idx, par, root, next;

    list = pl->buf[pl->cur];
    n = pl->count;
    copy_str(needle, sizeof(needle), filter);
    for (i = 0; needle[i] != '\0'; i++) {
        needle[i] = (char)tolower((unsigned char)needle[i]);
    }
    g_sort_key = sort_key;
    g_sort_desc = sort_desc;

    if (!tree) {
        /* Flat list: filter, then sort */
        pl->view_count = 0;
        for (i = 0; i < n; i++) {
            list[i].tree[0] = '\0';
            list[i].hidden = 0;
            if (matches(&list[i], needle)) {
                pl->view[pl->view_count++] = &list[i];
            }
        }
        qsort(pl->view, pl->view_count, sizeof(pl->view[0]), cmp_view);
        return;
    }

    /* Tree: sort everything first, then hang each process under its parent.
       Going through the sorted list backwards and pushing each child on the front of
       its parent's list leaves every list in sorted order. */
    for (i = 0; i < n; i++) {
        sorted_ptrs[i] = &list[i];
        child_head[i] = -1;
        child_next[i] = -1;
    }
    qsort(sorted_ptrs, n, sizeof(sorted_ptrs[0]), cmp_view);

    root = -1;
    for (i = n - 1; i >= 0; i--) {
        idx = (int)(sorted_ptrs[i] - list);
        par = find_pid_index(list, n, sorted_ptrs[i]->ppid);
        if (par >= 0 && par != idx) {
            child_next[idx] = child_head[par];
            child_head[par] = idx;
        } else {
            child_next[idx] = root;      /* parent not in the list (pid 1, kthreadd): a root */
            root = idx;
        }
    }

    g_list = list;
    copy_str(g_needle, sizeof(g_needle), needle);
    g_pl = pl;
    g_n = 0;
    for (i = root; i != -1; i = next) {
        next = child_next[i];
        tree_walk(i, "", next == -1, 1, 0);
    }
    pl->view_count = g_n;
}

int proc_find_in_view(const struct ProcList *pl, int pid) {
    int i;
    for (i = 0; i < pl->view_count; i++) {
        if (pl->view[i]->pid == pid) return i;
    }
    return -1;
}

/* ---------- Details popup ---------- */

void proc_read_detail(const struct ProcList *pl, int pid, struct ProcDetail *d) {
    char path[48], line[256];
    const struct ProcInfo *list;
    FILE *f;
    DIR *dir;
    struct dirent *e;
    ssize_t len;
    size_t n, i;
    int k;

    memset(d, 0, sizeof(*d));
    d->fd_count = -1;
    copy_str(d->exe, sizeof(d->exe), "?");
    copy_str(d->cwd, sizeof(d->cwd), "?");
    copy_str(d->parent, sizeof(d->parent), "?");

    /* exe and cwd are symbolic links; reading them needs permission for other users' processes */
    sprintf(path, "/proc/%d/exe", pid);
    len = readlink(path, d->exe, sizeof(d->exe) - 1);
    if (len > 0) d->exe[len] = '\0'; else copy_str(d->exe, sizeof(d->exe), "(not permitted)");
    sanitize(d->exe);

    sprintf(path, "/proc/%d/cwd", pid);
    len = readlink(path, d->cwd, sizeof(d->cwd) - 1);
    if (len > 0) d->cwd[len] = '\0'; else copy_str(d->cwd, sizeof(d->cwd), "(not permitted)");
    sanitize(d->cwd);

    /* Full command line: NUL-separated arguments */
    sprintf(path, "/proc/%d/cmdline", pid);
    n = 0;
    f = fopen(path, "r");
    if (f != NULL) {
        n = fread(d->cmdline, 1, sizeof(d->cmdline) - 1, f);
        fclose(f);
    }
    d->cmdline[n] = '\0';
    for (i = 0; i + 1 < n; i++) {
        if (d->cmdline[i] == '\0') d->cmdline[i] = ' ';
    }
    while (n > 0 && (d->cmdline[n - 1] == '\0' || d->cmdline[n - 1] == ' ')) {
        d->cmdline[--n] = '\0';
    }
    sanitize(d->cmdline);

    /* Memory figures: lines like "VmRSS:     1234 kB" */
    sprintf(path, "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (f != NULL) {
        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "VmSize:", 7) == 0) sscanf(line, "VmSize: %lu", &d->vm_size_kb);
            else if (strncmp(line, "VmRSS:", 6) == 0) sscanf(line, "VmRSS: %lu", &d->vm_rss_kb);
            else if (strncmp(line, "VmSwap:", 7) == 0) sscanf(line, "VmSwap: %lu", &d->vm_swap_kb);
        }
        fclose(f);
    }

    /* Open files: one entry per descriptor in /proc/<pid>/fd */
    sprintf(path, "/proc/%d/fd", pid);
    dir = opendir(path);
    if (dir != NULL) {
        d->fd_count = 0;
        while ((e = readdir(dir)) != NULL) {
            if (e->d_name[0] != '.') d->fd_count++;
        }
        closedir(dir);
    }

    /* Parent and children come from the list we already have */
    list = pl->buf[pl->cur];
    for (k = 0; k < pl->count; k++) {
        if (list[k].pid == pid) d->ppid = list[k].ppid;
        if (list[k].ppid == pid) d->children++;
    }
    for (k = 0; k < pl->count; k++) {
        if (list[k].pid == d->ppid) {
            copy_str(d->parent, sizeof(d->parent), list[k].comm);
            break;
        }
    }
}
