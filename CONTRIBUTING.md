# Contributing

Thanks for your interest in `blk-monitor`. The project is intentionally small
(single-file C, no dependencies), so contributions should keep that scope.

## Building

```bash
make            # Release: ./blk_monitor
make debug      # Debug: ./blk_monitor-debug
make asan       # ASan/UBSan: ./blk_monitor-asan
make check      # Unit and smoke tests
make check-asan # Sanitizer unit and smoke tests
make check-e2e  # Loopback tests when available
make lint       # ShellCheck and clang-tidy
make clean      # Remove generated binaries
```

Releases must compile cleanly under the release `CFLAGS` — no warnings, no
suppressions.

## Running the test suite

```bash
make check
```

The suite covers parsing, activity detection, CLI plumbing, output failures,
and argument validation. Loopback-device end-to-end checks run when `losetup`
and root or non-interactive `sudo` are available. Missing loopback
prerequisites are reported as skipped locally and in CI.

## Code style

- Follow the style already in `blk_monitor.c`: 4-space indent, K&R braces,
  `snake_case` for functions and locals, `PascalCase` for typedef'd structs,
  `SCREAMING_SNAKE` for `#define` constants.
- Use bounded library calls (`snprintf`, `strtol`/`strtoull` with error
  checking). Never introduce `strcpy`, `strcat`, `sprintf`, `gets`, `system`,
  `popen`, or `exec*`.
- All file descriptors must be paired with explicit `close()` on every exit
  path.
- Validate every error-returning syscall.

## Pull requests

1. Open an issue first for non-trivial changes so we can align on scope.
2. Keep PRs focused — one logical change per PR.
3. Update `README.md`, `blk_monitor.1`, and `CHANGELOG.md` together with code
   changes that affect them.
4. Add a smoke-test case for new behaviors when feasible.
5. Run `make check`, `make check-asan`, and `make lint` locally when their
   required tools are available, then confirm CI is green.

## Releases

1. Bump `#define VERSION` in `blk_monitor.c` and the man-page header.
2. Add a `CHANGELOG.md` entry.
3. Commit, then `git tag -a vX.Y.Z -m "Version X.Y.Z"`.
4. `git push && git push --tags`.
5. Draft a GitHub release from the new tag, pasting the changelog entry.

## Reporting security issues

See `SECURITY.md`. Please use the private advisory flow, not the public
issue tracker.
