#include "blk_monitor_core.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BASE_FIELD_COUNT 11U
#define DISCARD_FIELD_COUNT 15U
#define FLUSH_FIELD_COUNT 17U
#define MAX_PARSED_FIELDS 64U

static bool counter_decreased(unsigned long long previous,
                              unsigned long long current) {
    return current < previous;
}

static unsigned long long counter_delta(unsigned long long previous,
                                        unsigned long long current) {
    return current >= previous ? current - previous : 0;
}

int block_stats_parse(const char *text, BlockStats *stats) {
    unsigned long long fields[MAX_PARSED_FIELDS] = {0};
    size_t count = 0;
    const char *cursor = text;

    if (text == NULL || stats == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (*cursor != '\0') {
        char *end = NULL;

        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '-' || *cursor == '+') {
            errno = EINVAL;
            return -1;
        }
        if (count == MAX_PARSED_FIELDS) {
            errno = EOVERFLOW;
            return -1;
        }

        errno = 0;
        fields[count] = strtoull(cursor, &end, 10);
        if (errno != 0 || end == cursor ||
            (*end != '\0' && !isspace((unsigned char)*end))) {
            errno = EINVAL;
            return -1;
        }
        count++;
        cursor = end;
    }

    if (count != BASE_FIELD_COUNT && count != DISCARD_FIELD_COUNT &&
        count < FLUSH_FIELD_COUNT) {
        errno = EINVAL;
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    stats->reads_completed = fields[0];
    stats->reads_merged = fields[1];
    stats->sectors_read = fields[2];
    stats->time_reading_ms = fields[3];
    stats->writes_completed = fields[4];
    stats->writes_merged = fields[5];
    stats->sectors_written = fields[6];
    stats->time_writing_ms = fields[7];
    stats->io_in_progress = fields[8];
    stats->time_io_ms = fields[9];
    stats->weighted_time_io_ms = fields[10];

    if (count >= DISCARD_FIELD_COUNT) {
        stats->discards_completed = fields[11];
        stats->discards_merged = fields[12];
        stats->sectors_discarded = fields[13];
        stats->time_discarding_ms = fields[14];
    }
    if (count >= FLUSH_FIELD_COUNT) {
        stats->flushes_completed = fields[15];
        stats->time_flushing_ms = fields[16];
    }
    stats->field_count = count;
    return 0;
}

BlockDelta block_stats_delta(const BlockStats *previous,
                             const BlockStats *current) {
    BlockDelta delta = {0};
    bool has_discard = previous->field_count >= DISCARD_FIELD_COUNT &&
                       current->field_count >= DISCARD_FIELD_COUNT;
    bool has_flush = previous->field_count >= FLUSH_FIELD_COUNT &&
                     current->field_count >= FLUSH_FIELD_COUNT;

    delta.counters_reset =
        counter_decreased(previous->reads_completed, current->reads_completed) ||
        counter_decreased(previous->sectors_read, current->sectors_read) ||
        counter_decreased(previous->writes_completed, current->writes_completed) ||
        counter_decreased(previous->sectors_written, current->sectors_written) ||
        counter_decreased(previous->weighted_time_io_ms,
                          current->weighted_time_io_ms) ||
        (has_discard &&
         (counter_decreased(previous->discards_completed,
                            current->discards_completed) ||
          counter_decreased(previous->sectors_discarded,
                            current->sectors_discarded) ||
          counter_decreased(previous->time_discarding_ms,
                            current->time_discarding_ms))) ||
        (has_flush &&
         (counter_decreased(previous->flushes_completed,
                            current->flushes_completed) ||
          counter_decreased(previous->time_flushing_ms,
                            current->time_flushing_ms)));

    delta.sectors_read =
        counter_delta(previous->sectors_read, current->sectors_read);
    delta.sectors_written =
        counter_delta(previous->sectors_written, current->sectors_written);

    delta.active =
        delta.counters_reset ||
        current->io_in_progress > 0 ||
        current->reads_completed != previous->reads_completed ||
        current->writes_completed != previous->writes_completed ||
        delta.sectors_read > 0 ||
        delta.sectors_written > 0 ||
        current->weighted_time_io_ms != previous->weighted_time_io_ms ||
        (has_discard &&
         (current->discards_completed != previous->discards_completed ||
          current->sectors_discarded != previous->sectors_discarded ||
          current->time_discarding_ms != previous->time_discarding_ms)) ||
        (has_flush &&
         (current->flushes_completed != previous->flushes_completed ||
          current->time_flushing_ms != previous->time_flushing_ms));

    return delta;
}
