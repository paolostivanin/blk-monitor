#ifndef BLK_MONITOR_CORE_H
#define BLK_MONITOR_CORE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    unsigned long long reads_completed;
    unsigned long long reads_merged;
    unsigned long long sectors_read;
    unsigned long long time_reading_ms;
    unsigned long long writes_completed;
    unsigned long long writes_merged;
    unsigned long long sectors_written;
    unsigned long long time_writing_ms;
    unsigned long long io_in_progress;
    unsigned long long time_io_ms;
    unsigned long long weighted_time_io_ms;
    unsigned long long discards_completed;
    unsigned long long discards_merged;
    unsigned long long sectors_discarded;
    unsigned long long time_discarding_ms;
    unsigned long long flushes_completed;
    unsigned long long time_flushing_ms;
    size_t field_count;
} BlockStats;

typedef struct {
    unsigned long long sectors_read;
    unsigned long long sectors_written;
    bool active;
    bool counters_reset;
} BlockDelta;

int block_stats_parse(const char *text, BlockStats *stats);
BlockDelta block_stats_delta(const BlockStats *previous, const BlockStats *current);

#endif
