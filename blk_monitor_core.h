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

typedef enum {
    DEVICE_RESOLVE_OK = 0,
    DEVICE_RESOLVE_NOT_FOUND,
    DEVICE_RESOLVE_NOT_BLOCK,
    DEVICE_RESOLVE_PATH_TOO_LONG,
    DEVICE_RESOLVE_SYSFS_UNAVAILABLE
} DeviceResolveResult;

/*
 * Map a block device name (e.g. "nvme0n1p1") to the whole-disk name and the
 * path of its readable statistics file below sysfs_root (normally "/sys").
 * Partitions resolve to their parent disk.
 */
DeviceResolveResult blk_sysfs_resolve(const char *sysfs_root,
                                      const char *device_name,
                                      char *base_name, size_t base_size,
                                      char *stat_path, size_t stat_size);

int block_stats_parse(const char *text, BlockStats *stats);
BlockDelta block_stats_delta(const BlockStats *previous, const BlockStats *current);

#endif
