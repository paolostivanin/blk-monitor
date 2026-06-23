# blk-monitor

`blk_monitor` is a small Linux CLI that watches block-device I/O counters and
exits after a configurable period with no observed activity. By default it
calls the system-wide `sync()` function after the idle threshold is reached.

It does **not** unmount filesystems, eject media, or power off hardware. An idle
result is not a safe-removal guarantee: unmount or eject the device before
physically disconnecting it.

## Features

- Pure C11; dynamically links only against libc
- Reads `/sys/block/<device>/stat` through one persistent file descriptor
- Supports whole devices and partitions such as `/dev/sdb1` and
  `/dev/nvme0n1p2`
- Understands legacy 11-field statistics plus discard, flush, and future
  trailing fields
- Detects read, write, discard, flush, in-progress, and counter-reset activity
- Human-readable interactive, verbose, quiet, and JSON output modes
- PIE, full RELRO, `BIND_NOW`, stack protection, and fortified libc calls in
  release builds

## Build and install

```bash
make                 # release: ./blk_monitor
make debug           # debug: ./blk_monitor-debug
make asan            # ASan/UBSan: ./blk_monitor-asan
make check           # unit tests and smoke tests
make check-asan      # sanitizer unit and smoke tests
make check-e2e       # loopback tests when privileges are available
make lint            # ShellCheck and clang-tidy

sudo make install
sudo make uninstall
```

Distro packagers can stage installation without changing the build:

```bash
make install DESTDIR=/tmp/package PREFIX=/usr
```

Only the release binary is installed.

## Usage

```text
USAGE: blk_monitor [OPTIONS] /dev/DEVICE

  -i, --interval SEC   Poll interval in seconds (1-60, default: 2)
  -t, --idle-time SEC  Required idle time in seconds (5-3600, default: 10)
  -s, --sync           Run system-wide sync after idle (default)
  -S, --no-sync        Do not run sync after idle
  -q, --quiet          Print only the final status line
  -v, --verbose        Print one status line per poll
  -j, --json           Print one JSON object per line
  -C, --no-color       Disable colors
  -h, --help           Show help
  -V, --version        Show version
```

Examples:

```bash
blk_monitor /dev/sdb
blk_monitor -i 1 -t 5 /dev/sdb1
blk_monitor --no-sync --quiet /dev/nvme0n1p2
blk_monitor --json --no-sync /dev/sdb | jq -c .
```

Monitoring and `sync()` do not require root. Do not grant this program
`CAP_SYS_ADMIN`.

## Output

When stdout is a terminal, the default mode displays an in-place progress bar.
When redirected, it emits plain line-oriented output without ANSI escapes or
carriage returns.

Quiet mode prints one final line:

```text
I/O idle for 10 seconds on sdb; system-wide sync completed. Device remains mounted; unmount/eject before removal.
```

JSON mode emits one sample per poll:

```json
{"timestamp":"2026-06-23T10:30:00+0200","device":"sdb","read_mb_s":0.00,"write_mb_s":0.00,"active":false,"idle_seconds":2,"idle_target":10,"io_in_progress":0,"counters_reset":false}
```

After any requested `sync()` has completed, it emits:

```json
{"event":"idle","device":"sdb","idle_seconds":10,"sync_performed":true,"sync_scope":"system","unmount_performed":false}
```

`sync_scope` is `system` because `sync()` flushes all mounted filesystems, not
only the monitored device. `unmount_performed` is always `false`.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | The observed I/O-idle threshold was reached |
| `1` | Invalid input, unavailable/malformed statistics, or output failure |
| `2` | Interrupted by `SIGINT` or `SIGTERM` before reaching the threshold |

## How idle detection works

The program resolves a partition to its parent block device through
`/sys/class/block`, then samples `/sys/block/<device>/stat`.

A poll is active when any of the following is observed:

- completed reads or writes changed;
- read or written sectors changed;
- discard counters changed when available;
- flush counters changed when available;
- I/O is currently in progress;
- weighted I/O time changed;
- counters decreased, indicating a reset or device reattachment.

The idle timer resets after active polls. Because sampling is discrete, the
reported idle duration can exceed the requested threshold by less than one poll
interval. For example, `-i 3 -t 5` reports completion after 6 seconds.

## Requirements and compatibility

- Linux with sysfs block statistics
- A C11 compiler for source builds
- libc at runtime

The parser accepts the Linux 11-field format, the 15-field discard format, and
17-or-more-field formats containing flush and possible future trailing fields.

## Testing

`make check` always runs parser/activity unit tests and non-privileged CLI
tests. Loopback-device tests run when `losetup` and root or non-interactive
`sudo` are available; otherwise they are reported as skipped. CI follows the
same policy, so unavailable loopback support does not fail the workflow.

## License and contribution

MIT; see `LICENSE`. See `CONTRIBUTING.md` for development and release guidance.
Report security issues privately as described in `SECURITY.md`.
