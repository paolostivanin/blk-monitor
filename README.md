# blk-monitor

Lightweight Linux CLI tool that monitors block-device I/O activity and safely calls `sync()` once the device is idle. Zero dependencies, single ~17 KB binary, reads directly from `/sys/block`.

## Features

- **Zero dependencies**: pure C11, links only against libc
- **Hardened**: PIE + full RELRO + `_FORTIFY_SOURCE=3` + `-fstack-protector-strong`
- **Configurable**: polling intervals `1–60 s`, idle timeouts `5–3600 s`
- **Minimal overhead**: one persistent FD on `/sys/block/<dev>/stat`, ~0.1 ms per poll
- **Partition aware**: pass `/dev/sdb1` or `/dev/nvme0n1p2` and the parent device is resolved automatically
- **Pipe friendly**: line-buffered stdout, `SIGPIPE` is ignored — `... | head -n 1` exits cleanly
- **Output modes**: colored progress bar (default), verbose scrolling, quiet, and machine-readable JSON

## Quick Start

```bash
# Compile
make

# Run with defaults (2 s poll interval, 10 s idle threshold)
sudo ./blk_monitor /dev/sdb

# Quick mode: check every second, eject after 5 s idle
sudo ./blk_monitor -i 1 -t 5 /dev/sdb

# Conservative: 5 s poll, 60 s idle threshold
sudo ./blk_monitor -i 5 -t 60 /dev/sdb

# Pass a partition — parent device is resolved automatically
sudo ./blk_monitor /dev/sdb1            # monitors /dev/sdb
sudo ./blk_monitor /dev/nvme0n1p2       # monitors /dev/nvme0n1
```

## Command Reference

```
USAGE: blk_monitor [OPTIONS] /dev/sdX

  -i, --interval SEC   Poll interval in seconds (1-60, default: 2)
  -t, --idle-time SEC  Required idle time in seconds (5-3600, default: 10)
  -s, --sync           Sync on exit (default: enabled)
  -S, --no-sync        Disable sync on exit
  -q, --quiet          Quiet mode (only final status)
  -v, --verbose        Verbose scrolling output (one line per poll)
  -j, --json           JSON output (one object per line)
  -C, --no-color       Disable ANSI colors (auto-disabled when piped)
  -h, --help           Show this help
  -V, --version        Show version
```

Out-of-range values for `-i` and `-t` are rejected with a clear error (exit 1) — they are no longer silently clamped.

## Exit Codes

| Code | Meaning |
|------|---------|
| `0`  | Device became idle (and was synced if `-s` is enabled) |
| `1`  | Error: invalid args, missing device, read failure, write failure |
| `2`  | Interrupted by `SIGINT` / `SIGTERM` before reaching the idle threshold |

## Output Modes

### Default (progress bar)

```
Monitoring /dev/sdb (Target Idle: 10s)
[====>               ] ACTIVITY R:    0.0 MB/s  W:   42.3 MB/s
```

The bar fills as consecutive idle polls accumulate. Once the idle threshold is reached:

```
Device idle for 10 seconds.
Syncing filesystems... Done.
✓ Safe to remove/unmount sdb
```

### Verbose (`-v`)

One line per poll, no bar — useful for scripted logs:

```
Monitoring /dev/sdb (Target Idle: 10s)
R: 0.00 MB/s | W: 42.30 MB/s | Active: 1 | Idle: 0/5
R: 0.00 MB/s | W: 0.00 MB/s | Active: 0 | Idle: 1/5
R: 0.00 MB/s | W: 0.00 MB/s | Active: 0 | Idle: 2/5
...
```

### Quiet (`-q`)

Suppresses the live UI; only the final "safe to remove" line is printed.

### JSON (`-j`)

One JSON object per line, plus a final `event` line when the idle threshold is reached:

```json
{"timestamp":"2026-05-21T14:32:25+0200","device":"sdb","read_mb_s":0.00,"write_mb_s":42.30,"active":true,"idle_seconds":0,"idle_target":10,"io_in_progress":1}
{"timestamp":"2026-05-21T14:32:27+0200","device":"sdb","read_mb_s":0.00,"write_mb_s":0.00,"active":false,"idle_seconds":2,"idle_target":10,"io_in_progress":0}
{"event":"idle","device":"sdb","synced":true}
```

stdout is line-buffered, so each line reaches the consumer immediately:

```bash
sudo ./blk_monitor -j --no-sync -i 1 -t 5 /dev/sdb | jq -c .
```

## Build & Install

```bash
make                          # Release build (-O3, hardening flags)
make debug                    # Debug build (-O0 -g, no optimization)
make asan                     # ASan + UBSan build for development
make check                    # Run the smoke test suite
sudo make install             # Install to /usr/local/bin (and man page)
sudo make uninstall           # Remove installed files
```

Distro packagers can override the install location:

```bash
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

## How It Works

1. Opens `/sys/block/<device>/stat` once and keeps the file descriptor open.
2. Every `interval` seconds, `lseek(0)` + `read()` the latest counters.
3. Computes per-poll deltas for sectors read / written and looks at `io_in_progress` and `weighted_time_io_ms` — a device is *idle* when all four are zero.
4. After `idle_time` seconds of consecutive idleness, calls `sync()` (unless `-S`) and exits.

If you pass a partition (e.g. `sdb1`, `nvme0n1p2`), the tool walks back to the parent block device automatically — `/sys/block` only exposes whole-device stats.

## Permissions

- The monitoring itself runs as a regular user (read-only access to `/sys/block/*/stat`).
- `sync()` requires root to flush all filesystem buffers. Without root, you'll see a warning and the sync becomes best-effort. Use `-S` to skip it entirely if you only want to observe.

## Performance

- **CPU**: <0.1 % at the default `-i 2`
- **Memory**: ~8 KB RSS
- **Per-poll I/O**: one `lseek` + one `read` of ~120 bytes
- **Binary size**: ~17 KB (static against the host libc layout)

## Requirements

- Linux kernel 2.6.26+ (for the 11-field `/sys/block/<dev>/stat` format)
- A C11 compiler (gcc or clang)

## Tips

- Both `/dev/sdb` and `/dev/sdb1` work — partitions resolve to the parent device.
- Pipe `-j` output into `jq`, a log shipper, or `tee` — `SIGPIPE` is handled, so consumers that close early won't kill the monitor.
- The progress bar and `verbose` output are mutually exclusive with `-j`; JSON wins.

## Troubleshooting

**"Could not access stats for /dev/sdX"** — verify the device exists (`ls /sys/block/`) and that you can read it; the message includes the underlying `strerror()`.

**"Warning: Running without root"** — `sync()` needs CAP_SYS_ADMIN. Use `sudo`, grant the capability (`sudo setcap cap_sys_admin+ep ./blk_monitor`), or pass `-S` to skip the sync.

**Colors look garbled** — colors are auto-disabled when stdout is not a TTY or when `NO_COLOR` is set; pass `-C` to disable explicitly.

**Idle is never reached** — some filesystems / encrypted volumes have background I/O. Raise `-t`, watch with `-v` to see what's still moving.

## License

MIT — see `LICENSE`.

## Contributing

See `CONTRIBUTING.md`. To report security issues privately, see `SECURITY.md`.
