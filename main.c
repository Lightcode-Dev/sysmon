#define _POSIX_C_SOURCE 200809L   /* before any #include: enables clock_gettime, select, termios */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/select.h>
#include "sysmon.h"
#include "proc.h"
#include "config.h"
#include "ui.h"

/* sig_atomic_t is the only type the C standard guarantees is safe to write
   from a signal handler. 'volatile' stops the compiler caching it in a
   register, and 'static' keeps it private to this file. */
static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig) {
    (void)sig;              /* unused; the cast silences -Wextra */
    keep_running = 0;
}

/* The process list holds about 2 MB, so it lives in static storage, not on the stack */
static struct ProcList procs;
static struct UiState ui;

static struct termios saved_term;
static int term_saved = 0;

/* Milliseconds on a clock that never goes backwards */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* By default the terminal waits for Enter and echoes what you type. Turn both
   off so single key presses reach us at once. ISIG stays on, so Ctrl+C still
   sends SIGINT. VMIN=0 and VTIME=0 make read() return immediately. */
static void enter_raw_mode(void) {
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &saved_term) != 0) return;   /* stdin is not a terminal */
    term_saved = 1;
    raw = saved_term;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void leave_raw_mode(void) {
    if (term_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_term);
}

/* Bytes read but not yet decoded. A key or mouse sequence can be split between two
   read() calls, so an unfinished one is kept here until the rest arrives. */
static unsigned char inbuf[256];
static int inlen = 0;

/* Turn the waiting bytes into key presses and mouse events. Keys and mouse reports
   arrive as escape sequences:
     ESC [ A / B / C / D       Up, Down, Right, Left
     ESC [ 5 ~ / 6 ~           Page Up, Page Down  (1~ Home, 4~ End)
     ESC [ < b ; x ; y M       mouse press (SGR format); a final m means release
   A lone ESC is the Escape key. Returns UI_* flags, or -1 if stdin has been closed. */
static int read_input(void) {
    int n, i, key, flags, end, b, x, y, base;
    char tmp[32];

    n = (int)read(STDIN_FILENO, inbuf + inlen, sizeof(inbuf) - (size_t)inlen);
    if (n == 0) return -1;
    if (n < 0) return 0;
    inlen += n;

    flags = 0;
    i = 0;
    while (i < inlen) {
        key = inbuf[i++];

        if (key == 27 && i < inlen && (inbuf[i] == '[' || inbuf[i] == 'O')) {
            if (inbuf[i] == '[' && i + 1 < inlen && inbuf[i + 1] == '<') {
                /* Mouse report: find the closing M or m */
                end = i + 2;
                while (end < inlen && inbuf[end] != 'M' && inbuf[end] != 'm') end++;
                if (end >= inlen) {              /* unfinished: keep it for the next read */
                    i--;
                    break;
                }
                if (end - (i + 2) < (int)sizeof(tmp)) {
                    memcpy(tmp, inbuf + i + 2, (size_t)(end - (i + 2)));
                    tmp[end - (i + 2)] = '\0';
                    if (sscanf(tmp, "%d;%d;%d", &b, &x, &y) == 3 && inbuf[end] == 'M') {
                        base = b & ~(4 | 8 | 16);             /* ignore shift/alt/ctrl */
                        if (base == 0 || base == 64 || base == 65) {
                            flags |= ui_handle_mouse(&ui, &procs, base, x, y);
                        }
                    }
                }
                i = end + 1;
                continue;
            }
            if (i + 1 >= inlen) {                /* "ESC [" with nothing after it yet */
                i--;
                break;
            }
            switch (inbuf[i + 1]) {
            case 'A': key = UIKEY_UP;    i += 2; break;
            case 'B': key = UIKEY_DOWN;  i += 2; break;
            case 'C': key = UIKEY_RIGHT; i += 2; break;
            case 'D': key = UIKEY_LEFT;  i += 2; break;
            case 'H': key = UIKEY_HOME;  i += 2; break;
            case 'F': key = UIKEY_END;   i += 2; break;
            case '1': case '3': case '4': case '5': case '6':
                /* ESC [ <digit> ~ */
                if (i + 2 >= inlen) { i--; goto stash; }
                if (inbuf[i + 2] == '~') {
                    switch (inbuf[i + 1]) {
                    case '1': key = UIKEY_HOME; break;
                    case '4': key = UIKEY_END;  break;
                    case '5': key = UIKEY_PGUP; break;
                    case '6': key = UIKEY_PGDN; break;
                    default:  key = 0;          break;   /* Delete etc.: ignore */
                    }
                    i += 3;
                }
                break;
            default:
                break;
            }
        }
        if (key == 13) key = UIKEY_ENTER;
        if (key == 8) key = UIKEY_BACKSPACE;
        if (key == 0) continue;

        flags |= ui_handle_key(&ui, &procs, key);
        if (flags & UI_QUIT) break;
        if (flags & UI_RESORT) {
            /* later keys in this batch may depend on the new order */
            proc_build_view(&procs, ui.sort_key, ui.sort_desc, ui.tree, ui.filter);
            flags &= ~UI_RESORT;
            flags |= UI_REDRAW;
        }
    }

stash:
    /* Keep any unfinished sequence for the next call */
    if (i < inlen) memmove(inbuf, inbuf + i, (size_t)(inlen - i));
    inlen = (i < inlen) ? inlen - i : 0;
    if (flags & UI_RESORT) {
        proc_build_view(&procs, ui.sort_key, ui.sort_desc, ui.tree, ui.filter);
        flags &= ~UI_RESORT;
        flags |= UI_REDRAW;
    }
    return flags;
}

int main(void) {
    struct SystemStatus current_status;
    struct Config cfg;
    struct timeval tv;
    fd_set rfds;
    long next, remain;
    int flags, stdin_open;

    signal(SIGINT, handle_signal);   /* Ctrl+C */
    signal(SIGTERM, handle_signal);  /* plain 'kill' */

    /* Zero every field so nothing is ever garbage, even if a read fails */
    memset(&current_status, 0, sizeof(current_status));
    ui_init(&ui);

    /* Settings: built-in defaults, then the user's config file on top */
    config_defaults(&cfg);
    config_load(&cfg);
    if (!ui_set_theme(&ui, cfg.theme) && cfg.warning[0] == '\0') {
        snprintf(cfg.warning, sizeof(cfg.warning), "unknown theme '%s' (try default, nord, gruvbox, dracula, mono)", cfg.theme);
    }
    ui.tree = cfg.tree;
    ui.sort_key = cfg.sort_key;
    ui.sort_desc = cfg.sort_desc;
    if (cfg.warning[0] != '\0') {
        snprintf(ui.msg, sizeof(ui.msg), "%s", cfg.warning);   /* shown in the footer until a key is pressed */
    }

    /* Buffer all output so each frame goes to the terminal in one write.
       Must be called before anything is printed. */
    setvbuf(stdout, NULL, _IOFBF, 65536);

    /* Take one throwaway sample first. CPU usage needs two samples to compare,
       so without this the first frame would show averages since boot. */
    get_system_status(&current_status);
    proc_update(&procs, current_status.num_cores, current_status.cpu_total_ticks,
                current_status.ram_total_gb);
    sleep(SAMPLE_INTERVAL_SEC);

    /* \033[?1049h switches to the alternate screen (like vim or htop do), so
       your shell history is untouched on exit. \033[?25l hides the cursor.
       \033[?1000h asks the terminal to report mouse clicks and the wheel, and
       \033[?1006h makes it use the SGR format, which has no coordinate limit. */
    printf("\033[?1049h\033[?25l");
    if (cfg.mouse) printf("\033[?1000h\033[?1006h");
    fflush(stdout);
    enter_raw_mode();
    stdin_open = 1;

    next = now_ms();
    while (keep_running) {
        get_system_status(&current_status);
        proc_update(&procs, current_status.num_cores, current_status.cpu_total_ticks,
                    current_status.ram_total_gb);
        proc_build_view(&procs, ui.sort_key, ui.sort_desc, ui.tree, ui.filter);
        draw_dashboard(&current_status, &ui, &procs);

        /* Wait until the next sample is due. select() sleeps until a key
           arrives or the time runs out, so keys feel instant. */
        next += SAMPLE_INTERVAL_SEC * 1000L;
        while (keep_running) {
            remain = next - now_ms();
            if (remain <= 0) break;
            tv.tv_sec = remain / 1000;
            tv.tv_usec = (remain % 1000) * 1000;
            FD_ZERO(&rfds);
            if (stdin_open) FD_SET(STDIN_FILENO, &rfds);
            if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
                flags = read_input();
                if (flags < 0) {
                    stdin_open = 0;            /* stdin closed: keep running, just without keys */
                } else if (flags & UI_QUIT) {
                    keep_running = 0;
                } else if (flags & UI_REDRAW) {
                    draw_dashboard(&current_status, &ui, &procs);
                }
            }
        }
        if (next < now_ms()) next = now_ms();   /* fell behind (e.g. suspended): do not catch up */
    }

    /* Turn the mouse reporting off, restore the keyboard, cursor and normal screen */
    if (cfg.mouse) printf("\033[?1006l\033[?1000l");
    leave_raw_mode();
    printf("\033[?25h\033[?1049l");
    printf("Shutting down system monitor gracefully...\n");
    return 0;
}
