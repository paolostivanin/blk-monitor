#!/usr/bin/env bash
# CLI and optional loopback-device tests for blk_monitor.

set -uo pipefail

export PATH="$PATH:/sbin:/usr/sbin"
cd "$(dirname "$0")/.." || exit 1

BIN=${BIN:-./blk_monitor}
REQUIRE_LOOPBACK=${REQUIRE_LOOPBACK:-0}
SKIP_LOOPBACK=${SKIP_LOOPBACK:-0}

if [[ ! -x "$BIN" ]]; then
    echo "FAIL: $BIN is not executable" >&2
    exit 1
fi

pass=0
fail=0
skipped=0

ok() {
    printf '  ok   %s\n' "$1"
    pass=$((pass + 1))
}

bad() {
    printf '  FAIL %s\n' "$1"
    fail=$((fail + 1))
}

skip() {
    printf '  skip %s (%s)\n' "$1" "$2"
    skipped=$((skipped + 1))
}

hdr() {
    printf '\n=== %s ===\n' "$1"
}

expect_exit() {
    local expected=$1 description=$2
    shift 2
    local output status

    output=$("$@" 2>&1)
    status=$?
    if [[ $status -eq $expected ]]; then
        ok "$description (exit $status)"
    else
        bad "$description (expected $expected, got $status)"
        printf '       output: %s\n' "$output"
    fi
}

expect_output_match() {
    local regex=$1 description=$2
    shift 2
    local output

    output=$("$@" 2>&1)
    if [[ $output =~ $regex ]]; then
        ok "$description"
    else
        bad "$description"
        printf '       output: %s\n' "$output"
    fi
}

hdr "CLI plumbing"
expect_exit 0 "--version exits 0" "$BIN" --version
expect_exit 0 "--help exits 0" "$BIN" --help
expect_output_match '^blk_monitor 1\.2\.0$' \
    "--version is 1.2.0" "$BIN" --version
expect_output_match 'does not unmount or eject' \
    "--help states the safety boundary" "$BIN" --help
expect_output_match -- '-j, --json' \
    "--help advertises JSON" "$BIN" --help
expect_output_match -- '-S, --no-sync' \
    "--help advertises no-sync" "$BIN" --help

hdr "Argument validation"
expect_exit 1 "missing device path" "$BIN"
expect_exit 1 "extra operand rejected" "$BIN" /dev/null extra
expect_exit 1 "-i abc rejected" "$BIN" -i abc /dev/null
expect_exit 1 "-i 0 rejected" "$BIN" -i 0 /dev/null
expect_exit 1 "-i 9999 rejected" "$BIN" -i 9999 /dev/null
expect_exit 1 "-t 0 rejected" "$BIN" -t 0 /dev/null
expect_exit 1 "-t 999999 rejected" "$BIN" -t 999999 /dev/null
expect_output_match 'does not exist' \
    "missing path has precise error" "$BIN" /tmp/blk-monitor-not-a-device
expect_output_match 'not a block device' \
    "regular file has precise error" "$BIN" /etc/hostname

hdr "Output failures"
if "$BIN" --help >/dev/full 2>/dev/null; then
    bad "--help detects /dev/full"
else
    ok "--help detects /dev/full"
fi
if "$BIN" --version >/dev/full 2>/dev/null; then
    bad "--version detects /dev/full"
else
    ok "--version detects /dev/full"
fi

have_privileged_losetup() {
    [[ $EUID -eq 0 ]] && return 0
    command -v sudo >/dev/null 2>&1 || return 1
    sudo -n true >/dev/null 2>&1
}

skip_loopback_group() {
    local reason=$1
    if [[ $REQUIRE_LOOPBACK == 1 ]]; then
        bad "required loopback tests unavailable ($reason)"
    else
        skip "loopback end-to-end" "$reason"
    fi
}

hdr "End-to-end loopback"
if [[ $SKIP_LOOPBACK == 1 ]]; then
    skip_loopback_group "SKIP_LOOPBACK=1"
elif ! command -v losetup >/dev/null 2>&1; then
    skip_loopback_group "losetup not installed"
elif ! have_privileged_losetup; then
    skip_loopback_group "needs root or non-interactive sudo for losetup"
else
    sudo_command=()
    if [[ $EUID -ne 0 ]]; then
        sudo_command=(sudo -n)
    fi

    image=$(mktemp --suffix=.img)
    loop_device=
    # shellcheck disable=SC2329 # Invoked by the EXIT trap.
    cleanup() {
        if [[ -n $loop_device ]]; then
            "${sudo_command[@]}" losetup -d "$loop_device" 2>/dev/null || true
        fi
        rm -f "$image"
    }
    trap cleanup EXIT

    truncate -s 16M "$image"
    loop_device=$("${sudo_command[@]}" losetup -f --show "$image" 2>/dev/null)
    if [[ -z $loop_device ]]; then
        skip_loopback_group "could not create a loopback device"
    else
        quiet_output=$("$BIN" -q --no-sync -i 1 -t 5 "$loop_device")
        quiet_status=$?
        quiet_lines=$(printf '%s\n' "$quiet_output" | wc -l)
        if [[ $quiet_status -eq 0 && $quiet_lines -eq 1 &&
              $quiet_output == *"Device remains mounted"* ]]; then
            ok "quiet mode emits one accurate final line"
        else
            bad "quiet mode emits one accurate final line"
            printf '       output: %s\n' "$quiet_output"
        fi

        plain_output=$("$BIN" --no-sync -i 1 -t 5 "$loop_device")
        if [[ $plain_output != *$'\033'* && $plain_output != *$'\r'* &&
              $plain_output == *"unmount/eject before physical removal"* ]]; then
            ok "redirected default output is plain line-oriented text"
        else
            bad "redirected default output is plain line-oriented text"
        fi

        if "$BIN" -v --no-sync -i 1 -t 5 "$loop_device" \
            >/dev/full 2>/dev/null; then
            bad "runtime output failure exits 1"
        else
            runtime_status=$?
            if [[ $runtime_status -eq 1 ]]; then
                ok "runtime output failure exits 1"
            else
                bad "runtime output failure exits 1 (got $runtime_status)"
            fi
        fi

        if command -v jq >/dev/null 2>&1; then
            json_output=$("$BIN" -j --no-sync -i 1 -t 5 "$loop_device")
            final_event=$(printf '%s\n' "$json_output" | tail -n 1)
            if printf '%s\n' "$json_output" | jq -e . >/dev/null 2>&1 &&
               printf '%s\n' "$final_event" |
                   jq -e '.event == "idle" and
                          .sync_performed == false and
                          .sync_scope == "system" and
                          .unmount_performed == false and
                          (has("synced") | not)' >/dev/null; then
                ok "JSON schema and no-sync final event are correct"
            else
                bad "JSON schema and no-sync final event are correct"
                printf '       final: %s\n' "$final_event"
            fi

            sync_event=$("$BIN" -j --sync -i 1 -t 5 "$loop_device" | tail -n 1)
            if printf '%s\n' "$sync_event" |
               jq -e '.event == "idle" and .sync_performed == true' >/dev/null; then
                ok "sync final event is emitted after sync completion"
            else
                bad "sync final event is emitted after sync completion"
            fi

            first_line_file=$(mktemp)
            set +o pipefail
            "$BIN" -j --no-sync -i 1 -t 30 "$loop_device" 2>/dev/null |
                head -n 1 >"$first_line_file"
            pipeline_status=("${PIPESTATUS[@]}")
            set -o pipefail
            producer_status=${pipeline_status[0]}
            consumer_status=${pipeline_status[1]}
            first_line=$(<"$first_line_file")
            rm -f "$first_line_file"
            if [[ $producer_status -eq 1 && $consumer_status -eq 0 ]] &&
               printf '%s\n' "$first_line" | jq -e . >/dev/null; then
                ok "SIGPIPE yields producer exit 1 and valid first JSON line"
            else
                bad "SIGPIPE yields producer exit 1 and valid first JSON line"
                printf '       producer=%s consumer=%s line=%s\n' \
                    "$producer_status" "$consumer_status" "$first_line"
            fi

            start_time=$(date +%s)
            first_line=$(timeout 3 "$BIN" -j --no-sync -i 1 -t 30 \
                "$loop_device" 2>/dev/null | head -n 1 || true)
            end_time=$(date +%s)
            if [[ -n $first_line && $((end_time - start_time)) -le 3 ]]; then
                ok "JSON output is line-buffered"
            else
                bad "JSON output is line-buffered"
            fi
        else
            if [[ $REQUIRE_LOOPBACK == 1 ]]; then
                bad "jq is required for loopback JSON tests"
            else
                skip "loopback JSON tests" "jq not installed"
            fi
        fi

        nondivisible=$("$BIN" -q --no-sync -i 3 -t 5 "$loop_device")
        if [[ $nondivisible == *"I/O idle for 6 seconds"* ]]; then
            ok "reported idle duration reflects polling granularity"
        else
            bad "reported idle duration reflects polling granularity"
        fi
    fi
fi

printf '\n----- results: %d pass, %d fail, %d skipped -----\n' \
    "$pass" "$fail" "$skipped"
exit $((fail > 0 ? 1 : 0))
