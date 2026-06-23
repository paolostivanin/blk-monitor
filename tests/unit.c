#include "blk_monitor_core.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(bool condition, const char *description) {
    if (condition) {
        printf("ok   %s\n", description);
    } else {
        fprintf(stderr, "FAIL %s\n", description);
        failures++;
    }
}

static BlockStats parse_or_fail(const char *text, const char *description) {
    BlockStats stats = {0};
    int result = block_stats_parse(text, &stats);
    check(result == 0, description);
    return stats;
}

int main(void) {
    BlockStats base = parse_or_fail(
        "1 2 3 4 5 6 7 8 0 10 11\n", "parse 11-field statistics");
    check(base.field_count == 11 && base.sectors_read == 3 &&
              base.sectors_written == 7,
          "map base fields");

    BlockStats discard = parse_or_fail(
        "1 2 3 4 5 6 7 8 0 10 11 12 13 14 15\n",
        "parse 15-field statistics");
    check(discard.field_count == 15 && discard.discards_completed == 12 &&
              discard.sectors_discarded == 14,
          "map discard fields");

    BlockStats flush = parse_or_fail(
        "1 2 3 4 5 6 7 8 0 10 11 12 13 14 15 16 17\n",
        "parse 17-field statistics");
    check(flush.field_count == 17 && flush.flushes_completed == 16,
          "map flush fields");

    BlockStats future = parse_or_fail(
        "1 2 3 4 5 6 7 8 0 10 11 12 13 14 15 16 17 18 19\n",
        "ignore future trailing fields");
    check(future.field_count == 19 && future.flushes_completed == 16,
          "retain known fields from future format");

    BlockStats invalid = {0};
    errno = 0;
    check(block_stats_parse("1 2 3\n", &invalid) == -1 && errno == EINVAL,
          "reject truncated statistics");
    check(block_stats_parse("1 2 x 4 5 6 7 8 9 10 11\n", &invalid) == -1,
          "reject malformed statistics");
    check(block_stats_parse(
              "18446744073709551616 2 3 4 5 6 7 8 9 10 11\n",
              &invalid) == -1,
          "reject overflowing statistics");
    check(block_stats_parse("-1 2 3 4 5 6 7 8 9 10 11\n", &invalid) == -1,
          "reject negative statistics");
    check(block_stats_parse("1 2 3 4 5 6 7 8 9 10 11 12\n", &invalid) == -1,
          "reject unsupported partial format");

    BlockStats previous = parse_or_fail(
        "10 0 100 0 20 0 200 0 0 0 300 5 0 50 0 7 0\n",
        "prepare previous activity fixture");
    BlockStats idle = previous;
    BlockDelta delta = block_stats_delta(&previous, &idle);
    check(!delta.active && !delta.counters_reset,
          "unchanged counters are idle");

    BlockStats read_active = previous;
    read_active.reads_completed++;
    delta = block_stats_delta(&previous, &read_active);
    check(delta.active, "read completion is activity");

    BlockStats discard_active = previous;
    discard_active.discards_completed++;
    delta = block_stats_delta(&previous, &discard_active);
    check(delta.active, "discard completion is activity");

    BlockStats discard_time_active = previous;
    discard_time_active.time_discarding_ms++;
    delta = block_stats_delta(&previous, &discard_time_active);
    check(delta.active, "discard time is activity");

    BlockStats flush_active = previous;
    flush_active.flushes_completed++;
    delta = block_stats_delta(&previous, &flush_active);
    check(delta.active, "flush completion is activity");

    BlockStats flush_time_active = previous;
    flush_time_active.time_flushing_ms++;
    delta = block_stats_delta(&previous, &flush_time_active);
    check(delta.active, "flush time is activity");

    BlockStats in_progress = previous;
    in_progress.io_in_progress = 1;
    delta = block_stats_delta(&previous, &in_progress);
    check(delta.active, "in-progress I/O is activity");

    BlockStats reset = previous;
    reset.reads_completed = 1;
    reset.sectors_read = 1;
    delta = block_stats_delta(&previous, &reset);
    check(delta.active && delta.counters_reset && delta.sectors_read == 0,
          "counter reset is activity without underflow");

    printf("%d unit-test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
