#define _POSIX_C_SOURCE 200809L   /* before any #include: enables vsnprintf etc. under -ansi */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "sysmon.h"
#include "ui.h"

/* ---------- Colors and themes ---------- */

/* C_RESET is fixed. Every other color is a two-byte placeholder (byte value 1, then a
   letter naming the role). emit() swaps it for the current theme's escape sequence just
   before output, so the theme can change while the program runs. Octal \001 is used
   because a hex escape would swallow a following letter A-F. */
#define C_RESET  "\033[0m"
#define C_BORDER "\001A"
#define C_TITLE  "\001B"
#define C_LABEL  "\001C"
#define C_VALUE  "\001D"
#define C_TRACK  "\001E"   /* the empty part of a bar */
#define C_GOOD   "\001F"
#define C_WARN   "\001G"
#define C_BAD    "\001H"
#define C_DOWN   "\001I"
#define C_UP     "\001J"
#define C_READ   "\001K"
#define C_WRITE  "\001L"
#define C_SELBG  "\001M"   /* background of the selected table row */
#define C_ROOT   "\001N"   /* user name "root" */

/* Bar characters: a thin horizontal line, so stacked bars do not merge into
   a wall. The empty part uses the same glyph in a dim color. For a plain-ASCII
   terminal use "|" and "-" instead. */
#define BAR_FULL  "\xe2\x94\x81"   /* U+2501 heavy horizontal line */
#define BAR_EMPTY "\xe2\x94\x81"

#define NUM_ROLES 14
#define NUM_GRAD 10

/* A theme gives a 256-color number for each role, in the order of the C_* macros above
   (border, title, label, value, track, good, warn, bad, down, up, read, write, selected
   background, root), plus the bar gradient from left to right. -1 means "no color". */
struct Theme {
    const char *name;
    int c[NUM_ROLES];
    int grad[NUM_GRAD];
};

static const struct Theme themes[] = {
    { "default",
      { 240,  81, 245, 255, 240,  77, 221, 203,  81, 213, 114, 215, 238, 174 },
      {  71,  77, 113, 149, 185, 221, 215, 209, 203, 196 } },
    { "nord",
      {  60, 110, 103, 188,  59, 108, 222, 167, 110, 139, 108, 173, 236, 167 },
      { 108, 108, 144, 180, 222, 222, 216, 173, 167, 131 } },
    { "gruvbox",
      { 241, 214, 245, 223, 239, 142, 214, 167, 109, 175, 108, 208, 237, 167 },
      { 142, 142, 178, 214, 214, 208, 208, 202, 167, 160 } },
    { "dracula",
      {  61, 141, 103, 255,  60,  84, 228, 203, 117, 212,  84, 215, 236, 203 },
      {  84,  84, 120, 156, 228, 222, 215, 209, 203, 203 } },
    { "mono",
      {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1 },
      {  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1 } }
};
#define NUM_THEMES ((int)(sizeof(themes) / sizeof(themes[0])))

static char theme_esc[NUM_ROLES][24];   /* the escape sequence for each role, in the active theme */
static int g_grad[NUM_GRAD];

/* Build the escape sequences for theme idx. Every color starts with "22;", which switches
   off bold and faint first, so the title's bold never leaks into the text after it. */
static void apply_theme(int idx) {
    const struct Theme *t = &themes[idx];
    int r;

    for (r = 0; r < NUM_ROLES; r++) {
        if (r == 12) {                                   /* selected row: background or reverse video */
            if (t->c[r] >= 0) sprintf(theme_esc[r], "\033[48;5;%dm", t->c[r]);
            else strcpy(theme_esc[r], "\033[7m");
        } else if (r == 4 && t->c[r] < 0) {              /* empty bar part with no colors: faint */
            strcpy(theme_esc[r], "\033[2m");
        } else if (t->c[r] >= 0) {
            sprintf(theme_esc[r], r == 1 ? "\033[1;38;5;%dm" : "\033[22;38;5;%dm", t->c[r]);
        } else {
            strcpy(theme_esc[r], r == 1 ? "\033[1m" : "\033[22;39m");
        }
    }
    for (r = 0; r < NUM_GRAD; r++) g_grad[r] = t->grad[r];
}

/* Write s to the terminal, replacing each color placeholder with the theme's escape */
static void emit(const char *s) {
    const char *start = s;
    int role;

    for (; *s != '\0'; s++) {
        if (*s == '\001' && s[1] != '\0') {
            fwrite(start, 1, (size_t)(s - start), stdout);
            s++;
            role = *s - 'A';
            if (role >= 0 && role < NUM_ROLES) fputs(theme_esc[role], stdout);
            start = s + 1;
        }
    }
    fputs(start, stdout);
}

/* printf that understands the color placeholders. Output longer than 4 KB is cut. */
static void pr(const char *fmt, ...) {
    char buf[4096];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
}

/* ---------- Terminal helpers ---------- */
static int g_rows = 24;    /* terminal size, refreshed every frame */
static int g_cols = 80;
static int g_limit = 23;   /* last row panels may draw on (the footer owns the final row) */

static void get_term_size(void) {
    struct winsize ws;
    g_rows = 24;
    g_cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        g_rows = ws.ws_row;
        g_cols = ws.ws_col;
    }
    g_limit = g_rows - 1;
}

/* Move the cursor to (row, col), 1-based. Returns 0 if that spot is off-screen,
   so callers skip drawing instead of scrolling the terminal. */
static int goto_rc(int row, int col) {
    if (row < 1 || row > g_limit || col < 1 || col > g_cols) return 0;
    pr("\033[%d;%dH", row, col);
    return 1;
}

/* printf at a screen position */
static void put(int row, int col, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    if (!goto_rc(row, col)) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emit(buf);
}

/* Print string s, n times (for drawing horizontal lines) */
static void rep(const char *s, int n) {
    while (n-- > 0) fputs(s, stdout);
}

/* ---------- Drawing pieces ---------- */

/* Rounded box with a title on the top edge and an optional right-hand label:
   ╭─ Title ───────── right ╮ */
static void box(int row, int col, int w, int h, const char *title,
                const char *right, const char *right_color) {
    int i, tl, rl, n;

    tl = (int)strlen(title);
    rl = (right != NULL) ? (int)strlen(right) : 0;
    /* dashes = width - "╭─ Title " - (" right ╮" or "╮") */
    n = (right != NULL) ? w - (3 + tl + 1) - (rl + 3) : w - (3 + tl + 1) - 1;
    if (n < 0) n = 0;

    if (goto_rc(row, col)) {
        pr(C_BORDER "\xe2\x95\xad\xe2\x94\x80 " C_TITLE "%s" C_BORDER " ", title);
        rep("\xe2\x94\x80", n);
        if (right != NULL) {
            pr(" %s%s" C_BORDER " \xe2\x95\xae" C_RESET, right_color, right);
        } else {
            pr("\xe2\x95\xae" C_RESET);
        }
    }
    for (i = 1; i < h - 1; i++) {
        put(row + i, col, C_BORDER "\xe2\x94\x82" C_RESET);
        put(row + i, col + w - 1, C_BORDER "\xe2\x94\x82" C_RESET);
    }
    if (goto_rc(row + h - 1, col)) {
        pr(C_BORDER "\xe2\x95\xb0");
        rep("\xe2\x94\x80", w - 2);
        pr("\xe2\x95\xaf" C_RESET);
    }
}

/* Bar of width bw. Filled cells take their color from their position, so a
   full bar fades green -> red, like btop. */
static void bar(int row, int col, int bw, float pct) {
    int filled, i, g;

    if (bw < 1) return;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    filled = (int)((pct / 100.0f) * bw + 0.5f);   /* +0.5 rounds to nearest */

    if (!goto_rc(row, col)) return;
    for (i = 0; i < bw; i++) {
        if (i < filled) {
            g = g_grad[i * NUM_GRAD / bw];
            if (g >= 0) pr("\033[22;38;5;%dm" BAR_FULL, g);
            else pr("\033[22;39m" BAR_FULL);
        } else {
            pr(C_TRACK BAR_EMPTY);
        }
    }
    pr(C_RESET);
}

/* Color for a usage percentage */
static const char *pct_col(float pct) {
    if (pct < 50.0f) return C_GOOD;
    if (pct < 80.0f) return C_WARN;
    return C_BAD;
}

static const char *temp_col(int c) {
    if (c < 60) return C_GOOD;
    if (c < 75) return C_WARN;
    return C_BAD;
}

/* ---------- Text formatting ---------- */

/* bytes/second -> "3.4 MB/s" (out needs 16 bytes) */
static void format_rate(char *out, float bps) {
    if (bps >= 1073741824.0f) {
        sprintf(out, "%.1f GB/s", bps / 1073741824.0f);
    } else if (bps >= 1048576.0f) {
        sprintf(out, "%.1f MB/s", bps / 1048576.0f);
    } else if (bps >= 1024.0f) {
        sprintf(out, "%.1f KB/s", bps / 1024.0f);
    } else {
        sprintf(out, "%.0f B/s", bps);
    }
}

/* GB -> "8.5G", "252G" or "1.8T" (out needs 16 bytes) */
static void format_size_short(char *out, float gb) {
    if (gb >= 1024.0f) {
        sprintf(out, "%.1fT", gb / 1024.0f);
    } else if (gb >= 100.0f) {
        sprintf(out, "%.0fG", gb);
    } else {
        sprintf(out, "%.1fG", gb);
    }
}

/* One row: name, then two labelled rates (used by network and disk I/O) */
static void rate_row(int row, int col, int iw, const char *name,
                     const char *sym1, const char *col1, float rate1,
                     const char *sym2, const char *col2, float rate2) {
    char r1[16], r2[16];
    int namew;

    format_rate(r1, rate1);
    format_rate(r2, rate2);
    namew = iw - 26;      /* 26 columns are taken by the two rates and their symbols */
    if (namew < 4) namew = 4;
    if (namew > 16) namew = 16;   /* keep the rates near the name on wide panels */
    put(row, col, C_LABEL "%-*.*s " "%s%s " C_VALUE "%10s " "%s%s " C_VALUE "%10s" C_RESET,
        namew, namew, name, col1, sym1, r1, col2, sym2, r2);
}

/* ---------- Panels. Each returns its height in rows. ---------- */

static int panel_cpu(const struct SystemStatus *st, int row, int col, int w) {
    int iw, h, i, n, per_row, cw, bw, ob, r, c, show_t, t;
    char temp[16];
    const char *tcol;

    iw = w - 4;                          /* inside the border, minus 1 space padding each side */
    n = st->num_cores;
    per_row = (iw >= 36) ? 2 : 1;        /* two cores per row when there is room */
    cw = (per_row == 2) ? (iw - 2) / 2 : iw;
    show_t = st->core_temps_valid && cw >= 26;   /* per-core temperature column, if there is room */
    bw = cw - 9 - (show_t ? 5 : 0);      /* "C11 " + bar + " 100%" + " 45C" */
    if (bw < 3) bw = 3;
    h = 2 + 1 + (n + per_row - 1) / per_row;

    if (st->cpu_temp_valid) {
        sprintf(temp, "%d C", st->cpu_temp);
        tcol = temp_col(st->cpu_temp);
    } else {
        strcpy(temp, "N/A");
        tcol = C_LABEL;
    }
    box(row, col, w, h, "CPU", temp, tcol);

    /* Overall bar */
    ob = iw - 4 - 1 - 6;                 /* "CPU " + bar + " 100.0%" */
    if (ob < 3) ob = 3;
    put(row + 1, col + 2, C_LABEL "CPU " C_RESET);
    bar(row + 1, col + 6, ob, st->cpu_usage_pct);
    put(row + 1, col + 6 + ob + 1, "%s%5.1f%%" C_RESET,
        pct_col(st->cpu_usage_pct), st->cpu_usage_pct);

    /* Per-core bars */
    for (i = 0; i < n; i++) {
        r = row + 2 + i / per_row;
        c = col + 2 + (i % per_row) * (cw + 2);
        put(r, c, C_LABEL "C%-2d " C_RESET, i);
        bar(r, c + 4, bw, st->core_usage_pct[i]);
        put(r, c + 4 + bw + 1, "%s%3.0f%%" C_RESET,
            pct_col(st->core_usage_pct[i]), st->core_usage_pct[i]);
        if (show_t) {
            t = st->core_temp[i];
            if (t >= 0) {
                put(r, c + 4 + bw + 1 + 4, "%s%4dC" C_RESET, temp_col(t), t);
            }
        }
    }
    return h;
}

static int panel_mem(const struct SystemStatus *st, int row, int col, int w) {
    int iw, bw;
    float used_pct;

    iw = w - 4;
    bw = iw - 5 - 1 - 6;                 /* "Used " + bar + " 100.0%" */
    if (bw < 3) bw = 3;

    if (st->ram_total_gb > 0.0f) {
        used_pct = ((st->ram_total_gb - st->ram_avail_gb) / st->ram_total_gb) * 100.0f;
    } else {
        used_pct = 0.0f;
    }

    box(row, col, w, 5, "Memory", NULL, NULL);
    put(row + 1, col + 2, C_LABEL "Used " C_RESET);
    bar(row + 1, col + 7, bw, used_pct);
    put(row + 1, col + 7 + bw + 1, "%s%5.1f%%" C_RESET, pct_col(used_pct), used_pct);
    put(row + 2, col + 2, C_LABEL "Total " C_VALUE "%.2f GB" C_LABEL "   Avail " C_VALUE "%.2f GB" C_RESET,
        st->ram_total_gb, st->ram_avail_gb);
    put(row + 3, col + 2, C_LABEL "Free  " C_VALUE "%.2f GB" C_RESET, st->ram_free_gb);
    return 5;
}

static int panel_net(const struct SystemStatus *st, int row, int col, int w) {
    int iw, h, i;

    iw = w - 4;
    h = 2 + (st->num_ifaces > 0 ? st->num_ifaces : 1);
    box(row, col, w, h, "Network", NULL, NULL);
    if (st->num_ifaces == 0) {
        put(row + 1, col + 2, C_LABEL "(no interfaces)" C_RESET);
    }
    for (i = 0; i < st->num_ifaces; i++) {
        rate_row(row + 1 + i, col + 2, iw, st->ifaces[i].name,
                 "\xe2\x96\xbc", C_DOWN, st->ifaces[i].rx_rate,    /* down triangle */
                 "\xe2\x96\xb2", C_UP, st->ifaces[i].tx_rate);     /* up triangle   */
    }
    return h;
}

static int panel_disks(const struct SystemStatus *st, int row, int col, int w) {
    int iw, h, i, r, bw;
    char used_s[16], total_s[16], size[40];

    iw = w - 4;
    bw = iw - 8 - 1 - 5 - 1 - 13;        /* mount + bar + " 100%" + size text */
    if (bw < 4) bw = 4;
    h = 2 + (st->num_disks > 0 ? st->num_disks : 1) + st->num_diskio;
    box(row, col, w, h, "Disks", NULL, NULL);

    r = row + 1;
    if (st->num_disks == 0) {
        put(r, col + 2, C_LABEL "(no mounted disks)" C_RESET);
        r++;
    }
    for (i = 0; i < st->num_disks; i++) {
        format_size_short(used_s, st->disks[i].used_gb);
        format_size_short(total_s, st->disks[i].total_gb);
        sprintf(size, "%s/%s", used_s, total_s);
        put(r, col + 2, C_LABEL "%-8.8s " C_RESET, st->disks[i].mount);
        bar(r, col + 2 + 9, bw, st->disks[i].used_pct);
        put(r, col + 2 + 9 + bw + 1, "%s%3.0f%%" C_RESET " " C_VALUE "%s" C_RESET,
            pct_col(st->disks[i].used_pct), st->disks[i].used_pct, size);
        r++;
    }
    for (i = 0; i < st->num_diskio; i++) {
        rate_row(r, col + 2, iw, st->diskio[i].name,
                 "R", C_READ, st->diskio[i].read_rate,
                 "W", C_WRITE, st->diskio[i].write_rate);
        r++;
    }
    return h;
}

/* ---------- Process table ---------- */

static const char *sort_names[] = { "CPU%", "MEM%", "PID", "USER", "COMMAND", "IO" };

/* RES column: KB -> "512K", "12.3M", "345M", "1.2G" (out needs 16 bytes) */
static void format_kb_short(char *out, unsigned long kb) {
    if (kb >= 1048576UL) {
        sprintf(out, "%.1fG", kb / 1048576.0);
    } else if (kb >= 102400UL) {
        sprintf(out, "%.0fM", kb / 1024.0);
    } else if (kb >= 1024UL) {
        sprintf(out, "%.1fM", kb / 1024.0);
    } else {
        sprintf(out, "%luK", kb);
    }
}

/* Header cell color: bright for the column we are sorting by */
static const char *hcol(const struct UiState *ui, int key) {
    return (ui->sort_key == key) ? C_TITLE : C_LABEL;
}

/* How many screen columns a UTF-8 string takes: count every byte that is not a
   continuation byte (those look like 10xxxxxx) */
static int utf8_cols(const char *s) {
    int n = 0;
    for (; *s != '\0'; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    }
    return n;
}

/* IO column: bytes/second -> "0", "512B", "12.3K", "4.5M", "1.1G" (out needs 16 bytes) */
static void format_rate_short(char *out, float bps) {
    if (bps < 1.0f) {
        strcpy(out, "0");
    } else if (bps < 1024.0f) {
        sprintf(out, "%.0fB", bps);
    } else if (bps < 1048576.0f) {
        sprintf(out, "%.1fK", bps / 1024.0f);
    } else if (bps < 1073741824.0f) {
        sprintf(out, "%.1fM", bps / 1048576.0f);
    } else {
        sprintf(out, "%.1fG", bps / 1073741824.0f);
    }
}

/* One table row: 56 columns of fixed fields, then the tree prefix and the command.
   Inside a row only the text color changes (never a full reset), so the
   background of the selected row is not cut off in the middle. The selected
   row uses bright text everywhere so nothing fades into the highlight. */
static void proc_row(int r, int c, int cmdw, const struct ProcInfo *p, int selected) {
    char cpu[16], res[16], io[16], fold[16];
    const char *bg, *lab, *ucol, *scol, *ccol, *mcol, *icol, *tree;
    int threads, tcols, fcols, cw;

    if (selected) {
        bg = C_SELBG;
        lab = ucol = scol = ccol = mcol = icol = C_VALUE;
    } else {
        bg = "";
        lab = C_LABEL;
        ucol = (strcmp(p->user, "root") == 0) ? C_ROOT : C_LABEL;
        switch (p->state) {
        case 'R': scol = C_GOOD; break;
        case 'D': scol = C_WARN; break;
        case 'Z': scol = C_BAD; break;
        default:  scol = C_LABEL; break;
        }
        ccol = (p->cpu_pct < 0.05f) ? C_TRACK : pct_col(p->cpu_pct);
        mcol = (p->mem_pct >= 20.0f) ? C_BAD : (p->mem_pct >= 5.0f) ? C_WARN : C_LABEL;
        if (!p->io_ok || p->io_rate < 1.0f) icol = C_TRACK;
        else if (p->io_rate >= 1048576.0f) icol = C_WARN;
        else icol = C_LABEL;
    }

    sprintf(cpu, p->cpu_pct >= 100.0f ? "%.0f" : "%.1f", p->cpu_pct);
    format_kb_short(res, p->rss_kb);
    if (p->io_ok) format_rate_short(io, p->io_rate); else strcpy(io, "-");   /* "-": not allowed to look */
    threads = p->threads > 999 ? 999 : p->threads;

    /* The tree prefix is UTF-8, so printf cannot pad it by bytes: count columns ourselves */
    tree = p->tree;
    tcols = utf8_cols(tree);
    if (tcols > cmdw - 4) {            /* a very deep branch would leave no room for the name */
        tree = "";
        tcols = 0;
    }
    /* A folded branch shows how many processes it hides, e.g. "[+12] " */
    fold[0] = '\0';
    if (p->hidden > 0) sprintf(fold, "[+%d] ", p->hidden);
    fcols = (int)strlen(fold);
    cw = cmdw - tcols - fcols;
    if (cw < 0) cw = 0;

    put(r, c, "%s%s%7d " "%s%-9.9s " "%s%3d " "%s%c " "%s%3d " "%s%5s " "%s%5.1f " "%s%7s " "%s%7s "
              C_TRACK "%s" C_WARN "%s" C_VALUE "%-*.*s" C_RESET,
        bg, lab, p->pid, ucol, p->user, lab, p->nice, scol, p->state, lab, threads,
        ccol, cpu, mcol, p->mem_pct, lab, res, icol, io, tree, fold, cw, cw, p->cmd);
}

static void panel_procs(struct UiState *ui, const struct ProcList *pl,
                        int row, int col, int w, int h) {
    int iw, vis, i, sel, cmdw, count, max_scroll;
    char right[128];

    iw = w - 4;
    if (iw < 64) return;              /* too narrow for the columns */
    vis = h - 3;                      /* minus 2 border rows and the header row */
    count = pl->view_count;
    cmdw = iw - 56;
    ui->page_rows = vis;
    ui->tbl_hdr_y = row + 1;          /* remembered for mouse clicks */
    ui->tbl_y0 = row + 2;
    ui->tbl_x0 = col + 2;
    ui->tbl_rows = vis;

    if (ui->jump_top && count > 0) {
        /* The order just changed (sort or filter): start again from the first row */
        ui->sel_pid = pl->view[0]->pid;
        ui->sel_idx = 0;
        ui->scroll = 0;
    }
    ui->jump_top = 0;

    /* Find the selected process by pid. If it exited, stay on the same row. */
    sel = proc_find_in_view(pl, ui->sel_pid);
    if (sel < 0) {
        sel = ui->sel_idx;
        if (sel >= count) sel = count - 1;
        if (sel < 0) sel = 0;
    }
    if (count > 0) {
        ui->sel_pid = pl->view[sel]->pid;
        ui->sel_idx = sel;
    }

    /* Scroll just enough to keep the selection on screen */
    if (sel < ui->scroll) ui->scroll = sel;
    if (sel >= ui->scroll + vis) ui->scroll = sel - vis + 1;
    max_scroll = count - vis;
    if (max_scroll < 0) max_scroll = 0;
    if (ui->scroll > max_scroll) ui->scroll = max_scroll;
    if (ui->scroll < 0) ui->scroll = 0;

    if (ui->filter[0] != '\0') {
        snprintf(right, sizeof(right), "%d of %d  sort %s %s%s  filter: %s", count, pl->count,
                 sort_names[ui->sort_key], ui->sort_desc ? "desc" : "asc",
                 ui->tree ? "  tree" : "", ui->filter);
    } else {
        snprintf(right, sizeof(right), "%d processes  sort %s %s%s", pl->count,
                 sort_names[ui->sort_key], ui->sort_desc ? "desc" : "asc",
                 ui->tree ? "  tree" : "");
    }
    box(row, col, w, h, "Processes", right, C_LABEL);

    put(row + 1, col + 2,
        "%s%7s %s%-9s " C_LABEL "%3s %s " "%3s " "%s%5s %s%5s " C_LABEL "%7s " "%s%7s " "%s%s" C_RESET,
        hcol(ui, SORT_PID), "PID", hcol(ui, SORT_USER), "USER", "NI", "S", "THR",
        hcol(ui, SORT_CPU), "CPU%", hcol(ui, SORT_MEM), "MEM%", "RES",
        hcol(ui, SORT_IO), "IO/s", hcol(ui, SORT_NAME), "COMMAND");

    if (count == 0) {
        put(row + 2, col + 2, C_LABEL "(no matching processes)" C_RESET);
    }
    for (i = 0; i < vis && ui->scroll + i < count; i++) {
        proc_row(row + 2 + i, col + 2, cmdw, pl->view[ui->scroll + i],
                 ui->scroll + i == sel);
    }
}

/* ---------- Details popup ---------- */

static const char *state_name(char s) {
    switch (s) {
    case 'R': return "running";
    case 'S': return "sleeping";
    case 'D': return "waiting on disk";
    case 'Z': return "zombie (exited, not yet reaped)";
    case 'T': return "stopped";
    case 't': return "stopped by a debugger";
    case 'I': return "idle (kernel thread)";
    case 'X': return "dead";
    default:  return "unknown";
    }
}

/* One "Label    value" line, with the value cut to the width available */
static void detail_line(int r, int col, int vw, const char *label, const char *value) {
    put(r, col, C_LABEL "%-9s" C_VALUE "%.*s" C_RESET, label, vw, value);
}

static void draw_detail(const struct SystemStatus *st, const struct UiState *ui,
                        const struct ProcList *pl) {
    struct ProcDetail d;
    const struct ProcInfo *p, *list;
    char title[32], line[160], a[16], b[16], c[16], f[16], g[16];
    int i, w, h, row, col, vw, r, lines, cmdlen;
    long hz, age;

    list = pl->buf[pl->cur];
    p = NULL;
    for (i = 0; i < pl->count; i++) {
        if (list[i].pid == ui->detail_pid) { p = &list[i]; break; }
    }

    w = g_cols - 4;
    if (w > 100) w = 100;
    vw = w - 4 - 9;                     /* value width: inside the border, minus the label column */

    /* Height: the command line wraps over 1 to 3 rows, then 10 fixed rows and a hint line */
    lines = 1;
    if (p != NULL) {
        proc_read_detail(pl, p->pid, &d);
        cmdlen = (int)strlen(d.cmdline);
        lines = (cmdlen + vw - 1) / vw;
        if (lines < 1) lines = 1;
        if (lines > 3) lines = 3;
        h = 2 + lines + 10 + 1;
    } else {
        h = 6;
    }
    col = (g_cols - w) / 2 + 1;
    row = (g_limit - h) / 2 + 1;
    if (row < 1) row = 1;

    /* Blank the area first: the popup is drawn on top of the dashboard */
    for (r = row + 1; r < row + h - 1; r++) {
        put(r, col + 1, C_RESET "%*s", w - 2, "");
    }
    sprintf(title, "Process %d", ui->detail_pid);
    box(row, col, w, h, title, NULL, NULL);

    if (p == NULL) {
        put(row + 2, col + 2, C_WARN "This process has exited." C_RESET);
        put(row + h - 2, col + 2, C_LABEL "Esc to close" C_RESET);
        return;
    }

    /* Command line, wrapped over up to 3 lines */
    r = row + 1;
    for (i = 0; i < lines; i++) {
        put(r + i, col + 2, C_LABEL "%-9s" C_VALUE "%.*s" C_RESET, i == 0 ? "Command" : "",
            vw, d.cmdline + i * vw > d.cmdline + cmdlen ? "" : d.cmdline + i * vw);
    }
    r = row + 1 + lines;
    detail_line(r++, col + 2, vw, "Program", d.exe);
    detail_line(r++, col + 2, vw, "Folder", d.cwd);

    sprintf(line, "%c  (%s)", p->state, state_name(p->state));
    detail_line(r++, col + 2, vw, "State", line);

    sprintf(line, "%.1f%% of one core", p->cpu_pct);
    detail_line(r++, col + 2, vw, "CPU", line);

    format_kb_short(a, d.vm_rss_kb ? d.vm_rss_kb : p->rss_kb);
    format_kb_short(b, d.vm_size_kb);
    format_kb_short(c, d.vm_swap_kb);
    sprintf(line, "%.1f%% of RAM   resident %s   virtual %s   swap %s", p->mem_pct, a, b, c);
    detail_line(r++, col + 2, vw, "Memory", line);

    if (p->io_ok) {
        format_rate(a, p->io_read_rate);
        format_rate(b, p->io_write_rate);
        format_kb_short(f, p->io_read / 1024UL);
        format_kb_short(g, p->io_write / 1024UL);
        sprintf(line, "read %s   write %s   (total %s read, %s written)", a, b, f, g);
    } else {
        sprintf(line, "(not permitted)");
    }
    detail_line(r++, col + 2, vw, "Disk I/O", line);

    sprintf(line, "%s   nice %d   %d thread%s", p->user, p->nice, p->threads, p->threads == 1 ? "" : "s");
    detail_line(r++, col + 2, vw, "User", line);

    sprintf(line, "%d (%s)   %d child%s", d.ppid, d.parent, d.children, d.children == 1 ? "" : "ren");
    detail_line(r++, col + 2, vw, "Parent", line);

    if (d.fd_count >= 0) {
        sprintf(line, "%d open", d.fd_count);
    } else {
        sprintf(line, "(not permitted)");
    }
    detail_line(r++, col + 2, vw, "Files", line);

    hz = sysconf(_SC_CLK_TCK);
    age = st->uptime_seconds - (hz > 0 ? (long)(p->starttime / (unsigned long)hz) : 0L);
    if (age < 0) age = 0;
    sprintf(line, "%ldd %ldh %ldm %lds", age / 86400, (age % 86400) / 3600, (age % 3600) / 60, age % 60);
    detail_line(r++, col + 2, vw, "Running", line);

    put(row + h - 2, col + 2, C_VALUE "Esc" C_LABEL " close    " C_VALUE "x" C_LABEL " send a signal" C_RESET);
}

/* ---------- Keyboard ---------- */

void ui_init(struct UiState *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->sort_key = SORT_CPU;
    ui->sort_desc = 1;
    apply_theme(0);
}

int ui_set_theme(struct UiState *ui, const char *name) {
    int i;

    for (i = 0; i < NUM_THEMES; i++) {
        if (strcmp(themes[i].name, name) == 0) {
            ui->theme_idx = i;
            apply_theme(i);
            return 1;
        }
    }
    return 0;
}

/* Choose a sort column. Choosing the current one again flips the direction. */
static void set_sort(struct UiState *ui, int key) {
    if (ui->sort_key == key) {
        ui->sort_desc = !ui->sort_desc;
    } else {
        ui->sort_key = key;
        /* numbers start with the biggest, names and pids with the smallest */
        ui->sort_desc = (key == SORT_CPU || key == SORT_MEM || key == SORT_IO);
    }
    ui->jump_top = 1;
}

/* Move the selection to row idx of the view (clamped to the list) */
static void select_row(struct UiState *ui, const struct ProcList *pl, int idx) {
    if (pl->view_count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= pl->view_count) idx = pl->view_count - 1;
    ui->sel_pid = pl->view[idx]->pid;
    ui->sel_idx = idx;
}

static void send_signal(struct UiState *ui, int sig, const char *sig_name) {
    if (kill(ui->kill_pid, sig) == 0) {
        snprintf(ui->msg, sizeof(ui->msg), "Sent %s to %d (%s)",
                 sig_name, ui->kill_pid, ui->kill_name);
    } else {
        snprintf(ui->msg, sizeof(ui->msg), "Could not signal %d (%s): %s",
                 ui->kill_pid, ui->kill_name, strerror(errno));
    }
}

int ui_handle_key(struct UiState *ui, struct ProcList *pl, int key) {
    int flags, cur, len, page, i;
    const struct ProcInfo *list;

    flags = UI_REDRAW;
    ui->msg[0] = '\0';                 /* any key dismisses the last message */

    /* --- typing a filter --- */
    if (ui->mode == UI_MODE_FILTER) {
        len = (int)strlen(ui->filter);
        if (key == UIKEY_ENTER) {
            ui->mode = UI_MODE_NORMAL;           /* keep the filter */
        } else if (key == UIKEY_ESC) {
            ui->filter[0] = '\0';                /* drop the filter */
            ui->mode = UI_MODE_NORMAL;
            ui->jump_top = 1;
            flags |= UI_RESORT;
        } else if (key == UIKEY_BACKSPACE) {
            if (len > 0) ui->filter[len - 1] = '\0';
            ui->jump_top = 1;
            flags |= UI_RESORT;
        } else if (key >= 32 && key < 127 && len < (int)sizeof(ui->filter) - 1) {
            ui->filter[len] = (char)key;
            ui->filter[len + 1] = '\0';
            ui->jump_top = 1;
            flags |= UI_RESORT;
        }
        return flags;
    }

    /* --- choosing a signal for the process picked with x --- */
    if (ui->mode == UI_MODE_KILL) {
        if (key == 't') {
            send_signal(ui, SIGTERM, "SIGTERM");
        } else if (key == 'k') {
            send_signal(ui, SIGKILL, "SIGKILL");
        }
        ui->mode = UI_MODE_NORMAL;               /* any other key cancels */
        return flags;
    }

    /* --- the details popup --- */
    if (ui->mode == UI_MODE_DETAIL) {
        if (key == 'x') {
            ui->kill_pid = ui->detail_pid;
            ui->kill_name[0] = '\0';
            list = pl->buf[pl->cur];
            for (i = 0; i < pl->count; i++) {
                if (list[i].pid == ui->detail_pid) {
                    strncpy(ui->kill_name, list[i].comm, sizeof(ui->kill_name) - 1);
                    ui->kill_name[sizeof(ui->kill_name) - 1] = '\0';
                    break;
                }
            }
            ui->mode = UI_MODE_KILL;
        } else if (key == UIKEY_ESC || key == 'q' || key == UIKEY_ENTER || key == 'i') {
            ui->mode = UI_MODE_NORMAL;
        }
        return flags;
    }

    /* --- normal mode --- */
    cur = proc_find_in_view(pl, ui->sel_pid);
    if (cur < 0) cur = ui->sel_idx;
    page = ui->page_rows > 1 ? ui->page_rows - 1 : 1;

    switch (key) {
    case 'q':
        return UI_QUIT;
    case UIKEY_UP:   case 'k': select_row(ui, pl, cur - 1); break;
    case UIKEY_DOWN: case 'j': select_row(ui, pl, cur + 1); break;
    case UIKEY_PGUP:           select_row(ui, pl, cur - page); break;
    case UIKEY_PGDN:           select_row(ui, pl, cur + page); break;
    case UIKEY_HOME: case 'g': select_row(ui, pl, 0); break;
    case UIKEY_END:  case 'G': select_row(ui, pl, pl->view_count - 1); break;
    case UIKEY_RIGHT:
        set_sort(ui, (ui->sort_key + 1) % NUM_SORT_KEYS);
        flags |= UI_RESORT;
        break;
    case UIKEY_LEFT:
        set_sort(ui, (ui->sort_key + NUM_SORT_KEYS - 1) % NUM_SORT_KEYS);
        flags |= UI_RESORT;
        break;
    case 'c': set_sort(ui, SORT_CPU);  flags |= UI_RESORT; break;
    case 'm': set_sort(ui, SORT_MEM);  flags |= UI_RESORT; break;
    case 'p': set_sort(ui, SORT_PID);  flags |= UI_RESORT; break;
    case 'u': set_sort(ui, SORT_USER); flags |= UI_RESORT; break;
    case 'n': set_sort(ui, SORT_NAME); flags |= UI_RESORT; break;
    case 'o': set_sort(ui, SORT_IO);   flags |= UI_RESORT; break;
    case 'T':
        ui->theme_idx = (ui->theme_idx + 1) % NUM_THEMES;
        apply_theme(ui->theme_idx);
        snprintf(ui->msg, sizeof(ui->msg), "Theme: %s", themes[ui->theme_idx].name);
        break;
    case ' ':
        if (!ui->tree) {
            snprintf(ui->msg, sizeof(ui->msg), "Folding works in tree view: press t first");
        } else if (cur >= 0 && cur < pl->view_count) {
            proc_toggle_collapse(pl, pl->view[cur]->pid);
            flags |= UI_RESORT;
        }
        break;
    case 'r':
        ui->sort_desc = !ui->sort_desc;
        ui->jump_top = 1;
        flags |= UI_RESORT;
        break;
    case 't':
        ui->tree = !ui->tree;               /* the selection stays on the same process */
        flags |= UI_RESORT;
        break;
    case '/':
        ui->mode = UI_MODE_FILTER;
        break;
    case UIKEY_ENTER: case 'i':
        if (cur >= 0 && cur < pl->view_count) {
            ui->detail_pid = pl->view[cur]->pid;
            ui->mode = UI_MODE_DETAIL;
        }
        break;
    case 'x':
        if (cur >= 0 && cur < pl->view_count) {
            ui->kill_pid = pl->view[cur]->pid;
            strncpy(ui->kill_name, pl->view[cur]->comm, sizeof(ui->kill_name) - 1);
            ui->kill_name[sizeof(ui->kill_name) - 1] = '\0';
            ui->mode = UI_MODE_KILL;
        }
        break;
    default:
        break;
    }
    return flags;
}

/* Which sortable column is at x characters from the start of the table text?
   Offsets match the fixed column widths used in proc_row. Returns -1 for the others. */
static int column_at(int dx) {
    if (dx >= 0 && dx < 8) return SORT_PID;
    if (dx >= 8 && dx < 18) return SORT_USER;
    if (dx >= 28 && dx < 34) return SORT_CPU;
    if (dx >= 34 && dx < 40) return SORT_MEM;
    if (dx >= 48 && dx < 56) return SORT_IO;
    if (dx >= 56) return SORT_NAME;
    return -1;
}

int ui_handle_mouse(struct UiState *ui, struct ProcList *pl, int button, int x, int y) {
    int flags, cur, idx, key;

    flags = UI_REDRAW;
    if (ui->mode == UI_MODE_DETAIL) {              /* a click anywhere closes the popup */
        if (button == 0) ui->mode = UI_MODE_NORMAL;
        return flags;
    }
    if (ui->mode != UI_MODE_NORMAL) return 0;      /* typing a filter or choosing a signal: ignore */

    ui->msg[0] = '\0';
    cur = proc_find_in_view(pl, ui->sel_pid);
    if (cur < 0) cur = ui->sel_idx;

    if (button == 64) { select_row(ui, pl, cur - 3); return flags; }     /* wheel up */
    if (button == 65) { select_row(ui, pl, cur + 3); return flags; }     /* wheel down */
    if (button != 0 || ui->tbl_rows <= 0) return 0;

    if (y == ui->tbl_hdr_y) {                      /* click on a column header: sort by it */
        key = column_at(x - ui->tbl_x0);
        if (key >= 0) {
            set_sort(ui, key);
            flags |= UI_RESORT;
        }
        return flags;
    }
    if (y >= ui->tbl_y0 && y < ui->tbl_y0 + ui->tbl_rows) {
        idx = ui->scroll + (y - ui->tbl_y0);
        if (idx < pl->view_count) {
            if (pl->view[idx]->pid == ui->sel_pid) {   /* click the selected row again: details */
                ui->detail_pid = ui->sel_pid;
                ui->mode = UI_MODE_DETAIL;
            } else {
                select_row(ui, pl, idx);
            }
        }
        return flags;
    }
    return 0;
}

/* ---------- The whole frame ---------- */

static void draw_footer(const struct UiState *ui) {
    g_limit = g_rows;                    /* the footer may use the very last row */
    if (ui->mode == UI_MODE_FILTER) {
        put(g_rows, 1, C_TITLE "Filter: " C_VALUE "%s" C_TITLE "_" C_LABEL
            "   Enter keeps it, Esc clears it" C_RESET, ui->filter);
    } else if (ui->mode == UI_MODE_KILL) {
        put(g_rows, 1, C_WARN "Signal %d (%s)?  " C_VALUE "t" C_LABEL " = SIGTERM   "
            C_VALUE "k" C_LABEL " = SIGKILL   any other key cancels" C_RESET,
            ui->kill_pid, ui->kill_name);
    } else if (ui->mode == UI_MODE_DETAIL) {
        put(g_rows, 1, C_LABEL "Esc close   x send a signal" C_RESET);
    } else if (ui->msg[0] != '\0') {
        put(g_rows, 1, C_WARN "%.*s" C_RESET, g_cols - 1, ui->msg);
    } else {
        put(g_rows, 1, C_LABEL "%.*s" C_RESET, g_cols - 1,
            "q quit  j/k move  Enter details  c m p u n o sort  r reverse  t tree  Space fold  / filter  x kill  T theme");
    }
}

void draw_dashboard(const struct SystemStatus *st, struct UiState *ui,
                    const struct ProcList *pl) {
    int row, row_l, row_r, lw, rw, len, i, threads;
    int days, hours, minutes, seconds;
    char up[32], right[80];
    const struct ProcInfo *list;

    get_term_size();

    /* \033[?2026h starts a synchronized update (no flicker on terminals that
       support it, ignored by the rest). Then home the cursor and clear. */
    pr("\033[?2026h\033[H\033[2J");

    if (g_cols < 40 || g_rows < 12) {
        g_limit = g_rows;
        put(1, 1, "Terminal too small (need 40x12)");
        pr("\033[?2026l");
        fflush(stdout);
        return;
    }

    days = st->uptime_seconds / 86400;
    hours = (st->uptime_seconds % 86400) / 3600;
    minutes = (st->uptime_seconds % 3600) / 60;
    seconds = st->uptime_seconds % 60;
    sprintf(up, "%dd %dh %dm %ds", days, hours, minutes, seconds);

    /* Header: title on the left; uptime, process and thread counts on the right.
       Both counts come from the process list, so they agree with the table. */
    threads = 0;
    list = pl->buf[pl->cur];
    for (i = 0; i < pl->count; i++) threads += list[i].threads;

    put(1, 1, C_TITLE "sysmon" C_RESET);
    if (g_cols >= 80) {
        put(1, 9, C_LABEL "arch linux live dashboard" C_RESET);
    }
    len = sprintf(right, "up %s   procs %d   threads %d", up, pl->count, threads);
    put(1, g_cols - len + 1, C_LABEL "up " C_VALUE "%s" C_LABEL "   procs " C_VALUE "%d"
        C_LABEL "   threads " C_VALUE "%d" C_RESET, up, pl->count, threads);

    /* Layout: two columns on a wide terminal, one stacked column otherwise.
       Whatever room is left underneath goes to the process table. */
    if (g_cols >= 84) {
        lw = g_cols / 2;
        rw = g_cols - lw;
        row_l = 2;
        row_r = 2;
        row_l += panel_cpu(st, row_l, 1, lw);
        row_l += panel_mem(st, row_l, 1, lw);
        row_r += panel_net(st, row_r, lw + 1, rw);
        row_r += panel_disks(st, row_r, lw + 1, rw);
        row = (row_l > row_r) ? row_l : row_r;
    } else {
        row = 2;
        row += panel_cpu(st, row, 1, g_cols);
        row += panel_mem(st, row, 1, g_cols);
        row += panel_net(st, row, 1, g_cols);
        row += panel_disks(st, row, 1, g_cols);
    }
    ui->tbl_rows = 0;                    /* panel_procs sets this again if the table is drawn */
    if (g_limit - row + 1 >= 5) {        /* need a border, a header and a couple of rows */
        panel_procs(ui, pl, row, 1, g_cols, g_limit - row + 1);
    }

    if (ui->mode == UI_MODE_DETAIL) {
        draw_detail(st, ui, pl);
    }
    draw_footer(ui);

    pr(C_RESET "\033[?2026l");
    fflush(stdout);
}
