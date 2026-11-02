#ifndef PROC_H
#define PROC_H

#define MAX_PROCS 4096   /* processes beyond this many are ignored */

/* Sort columns */
#define SORT_CPU  0
#define SORT_MEM  1
#define SORT_PID  2
#define SORT_USER 3
#define SORT_NAME 4
#define SORT_IO   5
#define NUM_SORT_KEYS 6

#define MAX_COLLAPSED 64  /* tree branches that can be folded at once */

struct ProcInfo {
    int pid;
    int ppid;                  /* parent process id */
    char state;                /* R running, S sleeping, D disk wait, Z zombie, T stopped */
    int nice;
    int threads;
    unsigned long starttime;   /* with the pid, identifies a process (pids get reused) */
    unsigned long ticks;       /* CPU time used so far: user + system */
    unsigned long prev_ticks;  /* the same, at the previous sample */
    unsigned long rss_kb;      /* resident memory in KB */
    float cpu_pct;             /* percent of ONE core, so a busy multi-threaded process can pass 100 */
    float mem_pct;
    unsigned long io_read, io_write;           /* bytes read from / written to storage so far */
    unsigned long prev_io_read, prev_io_write; /* the same, at the previous sample */
    float io_read_rate, io_write_rate;         /* bytes per second */
    float io_rate;                             /* read + write */
    int io_ok;                 /* 1 if /proc/<pid>/io could be read (other users' processes need root) */
    int hidden;                /* tree view: how many descendants are folded away under this process */
    char user[16];
    char comm[16];             /* short name from the kernel */
    char cmd[96];              /* full command line, or [comm] for kernel threads */
    char tree[64];             /* branch drawing in front of the command in tree view, e.g. "|  `- " */
};

struct ProcList {
    struct ProcInfo buf[2][MAX_PROCS];  /* two buffers: this sample and the previous one */
    int cur;                            /* which buffer holds the latest sample */
    int count;                          /* processes in that buffer, sorted by pid */
    unsigned long prev_total_ticks;
    struct ProcInfo *view[MAX_PROCS];   /* what the table shows: filtered, then sorted */
    int view_count;
    int collapsed[MAX_COLLAPSED];       /* pids whose branch is folded in tree view */
    int n_collapsed;
};

/* Re-read /proc and work out each process's CPU% since the last call */
void proc_update(struct ProcList *pl, int num_cores, unsigned long total_ticks, float ram_total_gb);

/* Rebuild pl->view from the latest sample. An empty filter shows everything.
   With tree set, children are listed under their parent (siblings in sort order). */
void proc_build_view(struct ProcList *pl, int sort_key, int sort_desc, int tree, const char *filter);

/* Fold or unfold the branch under a process (tree view) */
void proc_toggle_collapse(struct ProcList *pl, int pid);

/* Position of a pid within pl->view, or -1 if it is not shown */
int proc_find_in_view(const struct ProcList *pl, int pid);

/* Extra facts about one process, read on demand for the details popup */
struct ProcDetail {
    char exe[160];             /* path of the running program */
    char cwd[160];             /* current working directory */
    char cmdline[384];         /* full command line */
    unsigned long vm_size_kb;  /* virtual memory */
    unsigned long vm_rss_kb;
    unsigned long vm_swap_kb;
    int fd_count;              /* open files, or -1 if we are not allowed to look */
    int ppid;
    char parent[16];           /* name of the parent process */
    int children;              /* how many processes have this one as their parent */
};

void proc_read_detail(const struct ProcList *pl, int pid, struct ProcDetail *d);

#endif
