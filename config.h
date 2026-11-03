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
#ifndef CONFIG_H
#define CONFIG_H

/* Settings read from ~/.config/sysmon/config (or $XDG_CONFIG_HOME/sysmon/config) */
struct Config {
    char theme[24];      /* theme name, checked later by the UI */
    int mouse;           /* 1 = enable mouse support */
    int tree;            /* 1 = start in tree view */
    int sort_key;        /* one of SORT_* from proc.h */
    int sort_desc;       /* 1 = biggest first */
    char hide_ifaces[128];   /* comma-separated interface patterns to hide, e.g. "veth*, docker0" */
    int hide_down;           /* 1 = also hide interfaces whose link is down */
    char warning[96];    /* first problem found in the file, or "" */
};

/* Fill in the built-in defaults */
void config_defaults(struct Config *cfg);

/* Apply the user's config file on top of the defaults. A missing file is fine. */
void config_load(struct Config *cfg);

#endif
