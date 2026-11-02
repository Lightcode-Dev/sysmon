#ifndef CONFIG_H
#define CONFIG_H

/* Settings read from ~/.config/sysmon/config (or $XDG_CONFIG_HOME/sysmon/config) */
struct Config {
    char theme[24];      /* theme name, checked later by the UI */
    int mouse;           /* 1 = enable mouse support */
    int tree;            /* 1 = start in tree view */
    int sort_key;        /* one of SORT_* from proc.h */
    int sort_desc;       /* 1 = biggest first */
    char warning[96];    /* first problem found in the file, or "" */
};

/* Fill in the built-in defaults */
void config_defaults(struct Config *cfg);

/* Apply the user's config file on top of the defaults. A missing file is fine. */
void config_load(struct Config *cfg);

#endif
