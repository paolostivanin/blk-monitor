# Contributing

Thanks for your interest in `blk-monitor`. The project is intentionally small
(single-file C, no dependencies), so contributions should keep that scope.

## Building

```bash
make            # Release build with full hardening
make debug      # Debug build with symbols
make asan       # AddressSanitizer + UBSan for development
make check      # Run the smoke test suite
make clean      # Remove the binary
```

Releases must compile cleanly under the release `CFLAGS` — no warnings, no
suppressions.

## Running the test suite

```bash
make check
```

The suite covers CLI plumbing and argument validation everywhere, plus
loopback-device end-to-end checks when `losetup` and root (or `sudo`) are
available. Loopback tests are skipped automatically if those prerequisites are
missing.

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
5. Run `make check` locally and confirm CI is green before requesting review.

## Releases

1. Bump `#define VERSION` in `blk_monitor.c`.
2. Add a `CHANGELOG.md` entry.
3. Commit, then `git tag -a vX.Y.Z -m "Version X.Y.Z"`.
4. `git push && git push --tags`.
5. Draft a GitHub release from the new tag, pasting the changelog entry.

## Reporting security issues

See `SECURITY.md`. Please use the private advisory flow, not the public
issue tracker.
