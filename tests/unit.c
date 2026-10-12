#define _XOPEN_SOURCE 700

#include "blk_monitor_core.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void make_dir(const char *root, const char *relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    for (char *cursor = path + strlen(root) + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            (void)mkdir(path, 0755);
            *cursor = '/';
        }
    }
    (void)mkdir(path, 0755);
}

static void make_file(const char *root, const char *relative) {
    char path[512];
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    file = fopen(path, "w");
    if (file != NULL) {
        fputs("1 2 3 4 5 6 7 8 0 10 11\n", file);
        fclose(file);
    }
}

/* Fake sysfs: devices/<bus>/<disk>[/<part>], class/block/<name> symlinks. */
static void add_disk(const char *root, const char *bus, const char *disk) {
    char relative[256];
    snprintf(relative, sizeof(relative), "devices/%s/%s", bus, disk);
    make_dir(root, relative);
    snprintf(relative, sizeof(relative), "block/%s", disk);
    make_dir(root, relative);
    snprintf(relative, sizeof(relative), "block/%s/stat", disk);
    make_file(root, relative);
    char link[512], target[512];
    snprintf(link, sizeof(link), "%s/class/block/%s", root, disk);
    snprintf(target, sizeof(target), "../../devices/%s/%s", bus, disk);
    (void)symlink(target, link);
}

static void add_partition(const char *root, const char *bus, const char *disk,
                          const char *part) {
    char relative[256], link[512], target[512];
    snprintf(relative, sizeof(relative), "devices/%s/%s/%s", bus, disk, part);
    make_dir(root, relative);
    snprintf(relative, sizeof(relative), "devices/%s/%s/%s/partition", bus,
             disk, part);
    make_file(root, relative);
    snprintf(link, sizeof(link), "%s/class/block/%s", root, part);
    snprintf(target, sizeof(target), "../../devices/%s/%s/%s", bus, disk, part);
    (void)symlink(target, link);
}

static void test_sysfs_resolve(void) {
    char root[] = "/tmp/blk-monitor-unit-XXXXXX";
    char base[64], stat_path[512], expected[512];
    DeviceResolveResult result;

    if (mkdtemp(root) == NULL) {
        check(false, "create fake sysfs root");
        return;
    }
    make_dir(root, "class/block");
    make_dir(root, "block");
    add_disk(root, "pci0000:00/usb1", "sdb");
    add_partition(root, "pci0000:00/usb1", "sdb", "sdb1");
    add_disk(root, "pci0000:00/nvme", "nvme0n1");
    add_partition(root, "pci0000:00/nvme", "nvme0n1", "nvme0n1p2");
    add_disk(root, "virtual", "dm-0");
    make_dir(root, "block/nodisk");
    make_dir(root, "devices/virtual/nodisk");
    {
        char link[512];
        snprintf(link, sizeof(link), "%s/class/block/nodisk", root);
        (void)symlink("../../devices/virtual/nodisk", link);
    }

    result = blk_sysfs_resolve(root, "sdb", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    snprintf(expected, sizeof(expected), "%s/block/sdb/stat", root);
    check(result == DEVICE_RESOLVE_OK && strcmp(base, "sdb") == 0 &&
              strcmp(stat_path, expected) == 0,
          "whole disk resolves to itself");

    result = blk_sysfs_resolve(root, "sdb1", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_OK && strcmp(base, "sdb") == 0 &&
              strcmp(stat_path, expected) == 0,
          "partition resolves to parent disk");

    result = blk_sysfs_resolve(root, "nvme0n1p2", base, sizeof(base),
                               stat_path, sizeof(stat_path));
    snprintf(expected, sizeof(expected), "%s/block/nvme0n1/stat", root);
    check(result == DEVICE_RESOLVE_OK && strcmp(base, "nvme0n1") == 0 &&
              strcmp(stat_path, expected) == 0,
          "nvme partition resolves to parent disk");

    result = blk_sysfs_resolve(root, "dm-0", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_OK && strcmp(base, "dm-0") == 0,
          "device-mapper name resolves to itself");

    result = blk_sysfs_resolve(root, "sdz", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_SYSFS_UNAVAILABLE,
          "unknown device is unavailable");

    result = blk_sysfs_resolve(root, "nodisk", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_SYSFS_UNAVAILABLE,
          "device without readable stat file is unavailable");

    result = blk_sysfs_resolve(root, "", base, sizeof(base), stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_NOT_FOUND, "empty name is rejected");

    result = blk_sysfs_resolve(root, "sdb1", base, 3, stat_path,
                               sizeof(stat_path));
    check(result == DEVICE_RESOLVE_PATH_TOO_LONG,
          "undersized name buffer is reported");

    result = blk_sysfs_resolve(root, "sdb", base, sizeof(base), stat_path, 8);
    check(result == DEVICE_RESOLVE_PATH_TOO_LONG,
          "undersized stat path buffer is reported");

    {
        char command[600];
        snprintf(command, sizeof(command), "rm -rf '%s'", root);
        if (system(command) != 0) {
            fprintf(stderr, "warning: could not remove %s\n", root);
        }
    }
}

int main(void) {
    test_sysfs_resolve();

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
