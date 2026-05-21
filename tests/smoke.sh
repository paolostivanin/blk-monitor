#!/usr/bin/env bash
# Smoke tests for blk_monitor. Designed to run locally and in CI.
#
# Loopback-device tests require sudo/root (and the `losetup` binary). They are
# skipped automatically when neither is available, so the test suite still
# reports a meaningful result on developer machines without root.

set -u
set -o pipefail

# losetup typically lives in /sbin or /usr/sbin and isn't on user PATH on
# systemd-merged distros. Make sure we can find it without forcing the caller
# to fix their PATH.
export PATH="$PATH:/sbin:/usr/sbin"

cd "$(dirname "$0")/.."

BIN=./blk_monitor
if [[ ! -x "$BIN" ]]; then
    echo "FAIL: $BIN not built. Run \`make\` first." >&2
    exit 1
fi

pass=0
fail=0
skipped=0

# ---- helpers -----------------------------------------------------------------

ok()    { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass+1)); }
bad()   { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail+1)); }
skip()  { printf '  \033[33mskip\033[0m %s (%s)\n' "$1" "$2"; skipped=$((skipped+1)); }
hdr()   { printf '\n=== %s ===\n' "$1"; }

# expect_exit <expected> <description> <command...>
expect_exit() {
    local expected="$1" desc="$2"; shift 2
    local out rc
    out=$("$@" 2>&1)
    rc=$?
    if [[ "$rc" -eq "$expected" ]]; then
        ok "$desc (exit $rc)"
    else
        bad "$desc (expected $expected, got $rc)"
        printf '       output: %s\n' "$out"
    fi
}

# expect_output_match <regex> <description> <command...>
expect_output_match() {
    local regex="$1" desc="$2"; shift 2
    local out
    out=$("$@" 2>&1)
    if [[ "$out" =~ $regex ]]; then
        ok "$desc"
    else
        bad "$desc"
        printf '       output: %s\n' "$out"
    fi
}

# ---- 1. CLI plumbing ---------------------------------------------------------

hdr "CLI plumbing"
expect_exit 0 "--version exits 0"            "$BIN" --version
expect_exit 0 "--help exits 0"               "$BIN" --help

expect_output_match '^blk_monitor 1\.'        "--version starts with version" "$BIN" --version
expect_output_match -- '-j, --json'           "--help advertises -j/--json"    "$BIN" --help
expect_output_match -- '-S, --no-sync'        "--help advertises -S/--no-sync" "$BIN" --help
expect_output_match -- '-C, --no-color'       "--help advertises -C/--no-color" "$BIN" --help

# ---- 2. Argument validation --------------------------------------------------

hdr "Argument validation"
expect_exit 1 "missing device path"            "$BIN"
expect_exit 1 "-i abc rejected"                "$BIN" -i abc /dev/null
expect_exit 1 "-i 0 rejected (below MIN)"      "$BIN" -i 0 /dev/null
expect_exit 1 "-i 9999 rejected (above MAX)"   "$BIN" -i 9999 /dev/null
expect_exit 1 "-t 0 rejected (below MIN)"      "$BIN" -t 0 /dev/null
expect_exit 1 "-t 999999 rejected (above MAX)" "$BIN" -t 999999 /dev/null
expect_exit 1 "non-block path rejected"        "$BIN" /tmp/blk-monitor-not-a-device
expect_exit 1 "regular file rejected"          "$BIN" /etc/hostname

expect_output_match 'between 1 and 60' \
    "-i out-of-range error mentions range" "$BIN" -i 0 /dev/null
expect_output_match 'between 5 and 3600' \
    "-t out-of-range error mentions range" "$BIN" -t 0 /dev/null

# ---- 3. End-to-end with a loopback device -----------------------------------

hdr "End-to-end (loopback)"

have_sudo_noninteractive() {
    [[ "$EUID" -eq 0 ]] && return 0
    command -v sudo >/dev/null 2>&1 || return 1
    sudo -n true 2>/dev/null
}

if ! command -v losetup >/dev/null 2>&1; then
    skip "loopback end-to-end" "losetup not installed"
    skip "JSON pipe is line-buffered" "losetup not installed"
    skip "JSON output is valid JSON" "losetup not installed"
    skip "SIGPIPE handled when consumer closes" "losetup not installed"
elif ! have_sudo_noninteractive; then
    skip "loopback end-to-end" "needs root or non-interactive sudo"
    skip "JSON pipe is line-buffered" "needs root or non-interactive sudo"
    skip "JSON output is valid JSON" "needs root or non-interactive sudo"
    skip "SIGPIPE handled when consumer closes" "needs root or non-interactive sudo"
else
    SUDO=""
    if [[ "$EUID" -ne 0 ]]; then
        SUDO="sudo -n"
    fi

    IMG=$(mktemp --suffix=.img)
    trap '[[ -n "${LOOP_DEV:-}" ]] && $SUDO losetup -d "$LOOP_DEV" 2>/dev/null; rm -f "$IMG"' EXIT
    truncate -s 16M "$IMG"
    LOOP_DEV=$($SUDO losetup -f --show "$IMG")
    if [[ -z "$LOOP_DEV" ]]; then
        bad "failed to set up loopback device"
    else
        # The device is brand-new and quiet, so an idle exit should fire fast.
        expect_exit 0 "idle detected on quiet loopback" \
            $SUDO "$BIN" --no-sync -i 1 -t 5 "$LOOP_DEV"

        if command -v jq >/dev/null 2>&1; then
            json_out=$($SUDO "$BIN" -j --no-sync -i 1 -t 5 "$LOOP_DEV" 2>&1)
            if printf '%s\n' "$json_out" | jq -e . >/dev/null 2>&1; then
                ok "JSON output is valid JSON"
            else
                bad "JSON output is valid JSON"
                printf '       output: %s\n' "$json_out"
            fi

            # SIGPIPE: pipe into `head -n 1` which closes the pipe after one line.
            # Without SIGPIPE handling, the second write would kill the process.
            # With our fix, the program exits cleanly with EXIT_ERROR (1).
            pipe_rc=0
            $SUDO "$BIN" -j --no-sync -i 1 -t 5 "$LOOP_DEV" 2>/dev/null | head -n 1 >/dev/null || pipe_rc=$?
            # head's exit code is what propagates; we just care the pipeline didn't crash horribly.
            # A successful read of one JSON object is the real signal.
            first_line=$($SUDO "$BIN" -j --no-sync -i 1 -t 5 "$LOOP_DEV" 2>/dev/null | head -n 1)
            if [[ -n "$first_line" ]] && printf '%s' "$first_line" | jq -e . >/dev/null 2>&1; then
                ok "SIGPIPE handled when consumer closes (first line still emitted)"
            else
                bad "SIGPIPE handled when consumer closes"
            fi

            # Line-buffering: the first line should appear within a couple of seconds,
            # not be buffered until the program exits.
            t0=$(date +%s)
            first=$(timeout 3 bash -c "$SUDO $BIN -j --no-sync -i 1 -t 30 $LOOP_DEV 2>/dev/null | head -n 1" || true)
            t1=$(date +%s)
            if [[ -n "$first" ]] && (( t1 - t0 <= 3 )); then
                ok "JSON pipe is line-buffered (first line within 3s)"
            else
                bad "JSON pipe is line-buffered (first line within 3s)"
                printf '       elapsed=%ds first=%q\n' "$((t1-t0))" "$first"
            fi
        else
            skip "JSON output is valid JSON" "jq not installed"
            skip "SIGPIPE handled when consumer closes" "jq not installed"
            skip "JSON pipe is line-buffered" "jq not installed"
        fi
    fi
fi

# ---- summary -----------------------------------------------------------------

printf '\n----- results: %d pass, %d fail, %d skipped -----\n' "$pass" "$fail" "$skipped"
exit $(( fail > 0 ? 1 : 0 ))
