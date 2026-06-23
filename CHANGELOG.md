# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.0] - 2026-06-23

### Changed
- Corrected the product contract: an idle result does not unmount, eject, power
  off, or guarantee safe physical removal.
- `sync()` remains enabled by default but is documented as unprivileged and
  system-wide; removed root and `CAP_SYS_ADMIN` guidance.
- The final JSON event is emitted after `sync()` and now reports
  `sync_performed`, `sync_scope`, and `unmount_performed`.
- Quiet mode emits exactly one accurate final status line; redirected default
  output is plain line-oriented text.
- Separate release, debug, sanitizer, unit, and sanitizer-unit artifacts prevent
  build variants from being reused or installed accidentally.

### Fixed
- All stdout paths now detect write and flush failures.
- Device validation now distinguishes missing paths, non-block files, overlong
  paths, and unavailable sysfs statistics without stale `errno` messages.
- Block-stat parsing supports 11, 15, and 17-or-more fields, including discard
  and flush activity.
- Counter decreases are treated as device resets/activity without unsigned
  underflow.
- Idle duration reports actual polling granularity when the threshold is not
  divisible by the interval.

### Testing
- Added unit coverage for legacy, discard, flush, future, malformed, overflow,
  reset, and activity scenarios.
- Expanded CLI tests for output failures, quiet/JSON contracts, plain redirected
  output, SIGPIPE status, and polling granularity.
- CI builds with GCC and Clang, runs sanitizers and static analysis, verifies
  hardening and installation, and skips unavailable loopback prerequisites.

## [1.1.2] - 2026-05-21

### Fixed
- `cleanup_terminal()` no longer writes a stray `\033[0m` ANSI reset and trailing
  newline to stdout when stdout is not a terminal. The escape sequence corrupted
  JSON (`-j`) and piped output, breaking downstream consumers such as `jq`.

## [1.1.1] - 2026-05-21

### Fixed
- `SIGPIPE` is now ignored, so piping output into a consumer that closes early
  (`... | head`, broken `tee`, log shipper restart) no longer kills the process.
- stdout is line-buffered, so `-j` / `-v` output reaches `jq` and other pipe
  consumers immediately instead of waiting for the 4–8 KB fully-buffered chunk.
- Out-of-range values for `-i` and `-t` are now rejected with a clear error and
  exit code 1 instead of being silently clamped to the min/max.
- `localtime()` failures in JSON mode no longer cause undefined behavior; the
  timestamp falls back to the raw epoch.
- JSON write failures (broken pipe with `SIGPIPE` ignored, disk full) cause a
  clean `EXIT_ERROR` instead of silently dropping records.

### Changed
- `idle-time < interval` still auto-corrects (it's a derived constraint), but
  now prints a one-line notice to stderr.

### Documentation
- README rewritten: corrected defaults (`2 s` / `10 s`), correct idle range
  (`5–3600 s`), documented `-j` / `-S` / `-C`, partition auto-resolution, exit
  code `2`, real example output, and removed the JSON-mode TODO that already
  shipped in 1.1.0.
- Added a man page (`blk_monitor.1`).
- Added `CHANGELOG.md`, `SECURITY.md`, `CONTRIBUTING.md`.

### Build
- Makefile honors `DESTDIR` and `PREFIX` for distro packaging.
- Added `uninstall`, `asan`, and `check` targets; added the `debug` target to
  `.PHONY`.
- Added a minimal GitHub Actions workflow that builds the release, debug, and
  sanitizer targets and runs the smoke test suite under loopback devices.

## [1.1.0] - 2026-04-03

### Added
- `-j, --json` machine-readable output mode (one JSON object per poll, plus a
  terminal `event` line on idle).
- Automatic partition → parent-device resolution: passing `/dev/sdb1` or
  `/dev/nvme0n1p2` now correctly monitors `/dev/sdb` or `/dev/nvme0n1`.
- Differentiated exit codes: `0` for idle, `1` for error, `2` for signal.

### Changed
- Persistent file descriptor on `/sys/block/<dev>/stat` with `lseek + read`,
  replacing `fopen`/`fclose` per poll. Per-poll overhead dropped from ~20 ms
  (bash equivalent) to <0.1 ms.
- `sync()` without root now emits a warning explaining buffers may not be
  fully flushed.
- All error messages now include the underlying `strerror(errno)`.
- Cursor restore in the terminal cleanup only runs when the cursor was
  actually hidden, preventing spurious escape codes in scripted contexts.

### Security
- Position-Independent Executable (`-fPIE` / `-pie`) and full RELRO
  (`-Wl,-z,relro,-z,now`) added to the production build.
- `sigaction()` failures now warn instead of failing silently.

## [1.0.0] - 2026-02-19

### Added
- Initial release: poll-based block-device I/O monitor with `sync()` on idle.
- CLI: `-i`, `-t`, `-s`, `-S`, `-q`, `-v`, `-C`, `-h`, `-V`.
- Color-coded progress bar UI with cursor-hidden full-screen mode.

[1.2.0]: https://github.com/paolostivanin/blk-monitor/compare/v1.1.2...v1.2.0
[1.1.2]: https://github.com/paolostivanin/blk-monitor/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/paolostivanin/blk-monitor/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/paolostivanin/blk-monitor/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/paolostivanin/blk-monitor/releases/tag/v1.0.0
