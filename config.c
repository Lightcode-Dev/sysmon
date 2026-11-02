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
#define _POSIX_C_SOURCE 200809L   /* before any #include: enables snprintf under -ansi */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "proc.h"
#include "config.h"

void config_defaults(struct Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->theme, "default");
    cfg->mouse = 1;
    cfg->tree = 0;
    cfg->sort_key = SORT_CPU;
    cfg->sort_desc = 1;
}

/* Remember only the first problem: one message is enough to point the user at the file */
static void warn(struct Config *cfg, int line, const char *what, const char *detail) {
    if (cfg->warning[0] == '\0') {
        snprintf(cfg->warning, sizeof(cfg->warning), "config line %d: %s '%s'", line, what, detail);
    }
}

/* Remove leading and trailing whitespace in place; returns the start of the result */
static char *trim(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* on/yes/true/1 -> 1, off/no/false/0 -> 0, anything else -> -1 */
static int parse_bool(const char *v) {
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0")) return 0;
    return -1;
}

static int parse_sort(const char *v) {
    if (!strcmp(v, "cpu")) return SORT_CPU;
    if (!strcmp(v, "mem") || !strcmp(v, "memory")) return SORT_MEM;
    if (!strcmp(v, "pid")) return SORT_PID;
    if (!strcmp(v, "user")) return SORT_USER;
    if (!strcmp(v, "name") || !strcmp(v, "command")) return SORT_NAME;
    if (!strcmp(v, "io") || !strcmp(v, "disk")) return SORT_IO;
    return -1;
}

void config_load(struct Config *cfg) {
    char path[512], line[256];
    const char *base, *home;
    char *key, *val, *hash, *eq;
    FILE *f;
    int n, b, k, i;

    base = getenv("XDG_CONFIG_HOME");
    home = getenv("HOME");
    if (base != NULL && base[0] != '\0') {
        snprintf(path, sizeof(path), "%s/sysmon/config", base);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(path, sizeof(path), "%s/.config/sysmon/config", home);
    } else {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL) return;                       /* no config file: use the defaults */

    n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        n++;
        hash = strchr(line, '#');                /* everything after # is a comment */
        if (hash != NULL) *hash = '\0';
        key = trim(line);
        if (key[0] == '\0') continue;
        eq = strchr(key, '=');
        if (eq == NULL) { warn(cfg, n, "expected key = value, got", key); continue; }
        *eq = '\0';
        val = trim(eq + 1);
        key = trim(key);
        for (i = 0; key[i] != '\0'; i++) key[i] = (char)tolower((unsigned char)key[i]);
        for (i = 0; val[i] != '\0'; i++) val[i] = (char)tolower((unsigned char)val[i]);

        if (!strcmp(key, "theme")) {
            if (strlen(val) < sizeof(cfg->theme)) strcpy(cfg->theme, val);
            else warn(cfg, n, "theme name too long:", val);
        } else if (!strcmp(key, "mouse")) {
            b = parse_bool(val);
            if (b < 0) warn(cfg, n, "mouse must be on or off, not", val); else cfg->mouse = b;
        } else if (!strcmp(key, "tree")) {
            b = parse_bool(val);
            if (b < 0) warn(cfg, n, "tree must be on or off, not", val); else cfg->tree = b;
        } else if (!strcmp(key, "sort")) {
            k = parse_sort(val);
            if (k < 0) warn(cfg, n, "unknown sort column", val); else cfg->sort_key = k;
        } else if (!strcmp(key, "sort_order")) {
            if (!strcmp(val, "desc")) cfg->sort_desc = 1;
            else if (!strcmp(val, "asc")) cfg->sort_desc = 0;
            else warn(cfg, n, "sort_order must be asc or desc, not", val);
        } else {
            warn(cfg, n, "unknown option", key);
        }
    }
    fclose(f);
}
