# sysmon

A live system monitor for the Linux terminal, written in C90. It reads
`/proc` and `/sys` directly and has no dependencies beyond the C library.

It shows overall and per-core CPU usage (with per-core temperatures on Intel),
memory, network throughput per interface, disk space and disk I/O, and an
interactive process table: sorting, filtering, a collapsible tree view, per-process
disk I/O, a details popup, signalling, mouse support and color themes. Bars fade from
green to red with load, and the layout adapts to the terminal size.

## Install

Arch Linux, from the AUR (the package is called sysmon-tui, the command is sysmon):

    yay -S sysmon-tui

Or build from source as described below.

## Build and run

You need `gcc` and `make` (on Arch: `sudo pacman -S base-devel`).

    make
    ./sysmon

Quit with `q` or Ctrl+C.

Install and uninstall (the prefix defaults to `/usr/local`):

    sudo make install
    sudo make uninstall

For packaging, `DESTDIR` and `PREFIX` are honoured and `CFLAGS` / `LDFLAGS` from the
environment are used (see the included `PKGBUILD`):

    make CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"
    make DESTDIR="$pkgdir" PREFIX=/usr install

The code is strict ANSI C (`-ansi -Wall -Wextra -pedantic`) and builds without warnings.

## Terminal requirements

A terminal with 256 colors and UTF-8 (kitty, foot, alacritty, most others). The bars and
borders use box-drawing characters; for a plain-ASCII look change `BAR_FULL` /
`BAR_EMPTY` at the top of `ui.c`. Minimum size is 40x12. The process table needs about
5 free rows under the panels, so use a tall window, or a wide one (84+ columns switches
to a two-column layout that leaves more room).

## Keys

| Key                    | Action                                              |
|------------------------|-----------------------------------------------------|
| `j` / `k`, arrows      | move the selection                                  |
| PgUp / PgDn, `g` / `G` | page, jump to top (`g` also un-picks the process) or bottom |
| `c` `m` `p` `u` `n` `o` | sort by CPU, memory, PID, user, name, disk I/O (again = flip) |
| Left / Right           | cycle the sort column                               |
| `r`                    | reverse the order                                   |
| `t`                    | toggle tree view                                    |
| `Space`                | fold or unfold the selected branch (tree view)      |
| `/`                    | filter by command, user or PID (Enter keeps, Esc clears) |
| `Enter` or `i`         | details popup for the selected process              |
| `x`                    | send a signal: `t` = SIGTERM, `k` = SIGKILL, other key cancels |
| `z`                    | pause or resume updates (look around without the list moving) |
| `T`                    | cycle the color theme                               |
| `q`                    | quit (closes the popup first if it is open)         |

Mouse: click a row to select it, click the selected row again for details, click a
column header to sort by it, scroll the wheel to move. A click anywhere closes the
popup. While mouse support is on, most terminals need Shift held to select text.

Ctrl+Z suspends sysmon and hands the terminal back in its normal state; `fg` brings it back. Resizing the
window redraws immediately.

Until you pick something, the highlight simply sits on the first row and the list moves under
it. Once you move it (`j`/`k`, arrows or a click) it follows that process by PID and stays on the
same screen row while the list re-sorts around it. `g` or Home returns to the top. Changing the
sort or filter also jumps back to the top. A folded branch shows
`[+N]`, the number of processes it hides; folding is ignored while a filter is active.

## Configuration

Copy `sysmon.conf.example` (installed in `/usr/share/doc/sysmon/`) to
`~/.config/sysmon/config`, or `$XDG_CONFIG_HOME/sysmon/config`. Every option is optional:

    theme = nord          # default, nord, gruvbox, dracula, mono
    mouse = on            # on or off
    tree = off            # start in tree view
    sort = cpu            # cpu, mem, pid, user, name, io
    sort_order = desc     # desc or asc
    hide_interfaces = veth*, docker0   # network interfaces to hide (a trailing * matches any ending)
    hide_down = off                    # on: also hide interfaces whose link is down

Problems in the file (an unknown option, a bad value) are reported in the footer on
start-up, and the rest of the file is still used. The `mono` theme uses no colors at all:
the selected row is shown in reverse video.

## How the numbers are made

- **CPU %**: change in `/proc/stat` idle vs. total ticks between two samples.
- **Per-process CPU %**: change in a process's user+system ticks, divided by the ticks one
  core could have used in that interval. It is percent of one core, so a busy
  multi-threaded process can pass 100.
- **Per-process disk I/O**: change in `read_bytes` + `write_bytes` from `/proc/<pid>/io`
  (real storage traffic; page-cache hits do not count). The kernel only lets you read your
  own processes unless you are root; the rest show `-`.
- **Per-core temperature**: the Intel `coretemp` driver has one sensor per physical core;
  each logical CPU is mapped to its core through `topology/core_id`, so hyperthread
  siblings show the same value. AMD's `k10temp` has no per-core sensors, so only the
  package temperature is shown.
- **Network and disk rates**: byte counters from `/proc/net/dev` and `/proc/diskstats`,
  differenced between samples and divided by the real elapsed time. Loopback, loop and ram devices are skipped.
- **Memory "used"**: total minus `MemAvailable`, like `free` and btop.
- **Swap**: `SwapTotal` minus `SwapFree` from `/proc/meminfo`. zram shows up here when it is used as swap.
- **Load averages**: the first three numbers of `/proc/loadavg`, shown in the header when it is wide enough.
- **CPU clock**: `scaling_cur_freq` in each CPU's `cpufreq` directory, shown next to each core when the panel
  is wide enough.
- **NVMe temperature**: every NVMe controller has a hwmon sensor whose `device` link names the controller, so
  the temperature of `nvme0` is shown on the `nvme0n1` row of the Disks panel. SATA drives are not covered yet.
- **Link dot** in front of each network interface, from `/sys/class/net/<name>/operstate`: green = up,
  red = down, hollow = unknown (loopback and some virtual devices report that).

## Source layout

| File          | Purpose                                                         |
|---------------|-----------------------------------------------------------------|
| `sysmon.c/.h` | collects CPU, temperatures, memory, network, disks              |
| `proc.c/.h`   | reads `/proc/<pid>`, per-process CPU and I/O, sorting, tree, details |
| `config.c/.h` | reads the config file                                           |
| `ui.c/.h`     | drawing (panels, themes, popup), key and mouse handling         |
| `main.c`      | terminal setup, main loop, decoding of keys and mouse reports   |

Limits: 64 CPUs, 8 network interfaces, 8 mounted filesystems, 8 disks, 4096 processes and
64 folded branches are tracked; the constants are at the top of `sysmon.h` and `proc.h`.

## Known limitations

- Linux only.
- Per-core temperatures need the `coretemp` driver; without it the column is hidden.
- Drive temperatures are shown for NVMe drives only.
- If `/proc` is mounted with `hidepid`, only your own processes are visible.

## License

sysmon is free software, released under the GNU General Public License, version 3 or (at your option) any later version. See the `LICENSE` file.
