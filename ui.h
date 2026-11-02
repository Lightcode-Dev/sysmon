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
#ifndef UI_H
#define UI_H

#include "sysmon.h"
#include "proc.h"

/* Key codes. Plain keys use their ASCII value; special keys use these. */
#define UIKEY_ESC       27
#define UIKEY_ENTER     10
#define UIKEY_BACKSPACE 127
#define UIKEY_UP        1001
#define UIKEY_DOWN      1002
#define UIKEY_RIGHT     1003
#define UIKEY_LEFT      1004
#define UIKEY_PGUP      1005
#define UIKEY_PGDN      1006
#define UIKEY_HOME      1007
#define UIKEY_END       1008

/* What a key press asks main to do (combined with |) */
#define UI_REDRAW 1   /* draw the screen again */
#define UI_RESORT 2   /* the sort or filter changed: rebuild the process view first */
#define UI_QUIT   4

/* Input modes */
#define UI_MODE_NORMAL 0
#define UI_MODE_FILTER 1   /* typing a filter after pressing / */
#define UI_MODE_KILL   2   /* waiting for a signal choice after pressing x */
#define UI_MODE_DETAIL 3   /* the details popup is open */

struct UiState {
    int sort_key;          /* one of SORT_* */
    int sort_desc;         /* 1 = biggest first */
    char filter[32];
    int mode;
    int tree;              /* 1 = show processes as a tree */
    int jump_top;          /* set when the order changes: go back to the first row */
    int detail_pid;        /* the process shown in the details popup */
    int theme_idx;         /* index into the built-in theme list */
    /* Where the process table was drawn last frame, so mouse clicks can be mapped to rows */
    int tbl_hdr_y;         /* screen row of the column headers */
    int tbl_y0;            /* screen row of the first data row */
    int tbl_x0;            /* screen column where the table text starts */
    int tbl_rows;          /* data rows that fit (0 = table not shown) */
    int sel_pid;           /* the highlighted process: tracked by pid so it stays put as the list re-sorts */
    int sel_idx;           /* its last known row, used if that process exits */
    int scroll;            /* first visible row of the process table */
    int page_rows;         /* rows the table showed last frame (for Page Up/Down) */
    int kill_pid;
    char kill_name[32];
    char msg[96];          /* one-line result of the last action, cleared by the next key */
};

void ui_init(struct UiState *ui);

/* Switch to a named theme (default, nord, gruvbox, dracula, mono). Returns 0 if unknown. */
int ui_set_theme(struct UiState *ui, const char *name);

/* Apply one key press. Returns UI_* flags telling main what to do next. */
int ui_handle_key(struct UiState *ui, struct ProcList *pl, int key);

/* Apply a mouse event. button: 0 = left click, 64 = wheel up, 65 = wheel down.
   x and y are 1-based screen coordinates. Returns UI_* flags. */
int ui_handle_mouse(struct UiState *ui, struct ProcList *pl, int button, int x, int y);

/* Draw one full frame, sized to the current terminal. */
void draw_dashboard(const struct SystemStatus *status, struct UiState *ui,
                    const struct ProcList *pl);

#endif
