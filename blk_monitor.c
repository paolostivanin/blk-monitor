/*
 * blk_monitor.c - Lightweight block device I/O monitor
 *
 * Monitors block device (Read & Write) activity and syncs when idle.
 *
 */

/* Required for sigaction, realpath, PATH_MAX, sync() when compiled with -std=c11 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <getopt.h>
#include <signal.h>
#include <libgen.h>
#include <fcntl.h>

/* Default configuration */
#define DEFAULT_POLL_INTERVAL 2
#define DEFAULT_IDLE_DURATION 10
#define MIN_POLL_INTERVAL 1
#define MAX_POLL_INTERVAL 60
#define MIN_IDLE_DURATION 5
#define MAX_IDLE_DURATION 3600

#define SECTOR_SIZE 512ULL
#define MB_DIVISOR (1024.0 * 1024.0)

#define VERSION "1.1.1"

#define EXIT_SUCCESS_IDLE 0
#define EXIT_ERROR        1
#define EXIT_SIGNAL       2

/* Configuration structure */
typedef struct {
    char device_path[PATH_MAX]; // User input path
    char sys_path[PATH_MAX];    // /sys/block path
    char device_name[256];      // sdb, sdc, etc.
    int poll_interval;
    int idle_duration;
    bool auto_sync;
    bool quiet;
    bool verbose;
    bool no_color;
    bool json_output;
} Config;

/* Global flag for signal handling */
static volatile sig_atomic_t keep_running = 1;
static bool cursor_hidden = false;

/* ANSI color codes */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define HIDE_CURSOR   "\033[?25l"
#define SHOW_CURSOR   "\033[?25h"

/* Stat structure matching /sys/block/X/stat */
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
} BlockStats;

static void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
}

static void setup_signals(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    // Remove SA_RESTART to allow sleep() to be interrupted immediately
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to set SIGINT handler: %s\n", strerror(errno));
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to set SIGTERM handler: %s\n", strerror(errno));
    }

    /* Ignore SIGPIPE so a downstream consumer closing the pipe (e.g. `... | head`)
     * surfaces as a write error we can handle, instead of killing the process. */
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    ign.sa_flags = 0;
    if (sigaction(SIGPIPE, &ign, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to ignore SIGPIPE: %s\n", strerror(errno));
    }
}

static void cleanup_terminal(void) {
    if (cursor_hidden) {
        fputs(SHOW_CURSOR, stdout);
        cursor_hidden = false;
    }
    fputs(COLOR_RESET, stdout);
    putchar('\n');
}

static void print_help(const char *program_name) {
    printf("USAGE: %s [OPTIONS] /dev/sdX\n", program_name);
    printf("Monitors Read & Write activity. Syncs and exits when idle.\n\n");
    printf("  -i, --interval SEC   Poll interval in seconds (%d-%d, default: %d)\n",
           MIN_POLL_INTERVAL, MAX_POLL_INTERVAL, DEFAULT_POLL_INTERVAL);
    printf("  -t, --idle-time SEC  Required idle time in seconds (%d-%d, default: %d)\n",
           MIN_IDLE_DURATION, MAX_IDLE_DURATION, DEFAULT_IDLE_DURATION);
    printf("  -s, --sync           Sync on exit (default: enabled)\n");
    printf("  -S, --no-sync        Disable sync on exit\n");
    printf("  -q, --quiet          Quiet mode\n");
    printf("  -v, --verbose        Verbose scrolling output\n");
    printf("  -j, --json           JSON output (one object per line)\n");
    printf("  -C, --no-color       Disable colors\n");
    printf("  -h, --help           Show this help\n");
    printf("  -V, --version        Show version\n");
}

static int parse_int_arg(const char *arg, const char *name, int min, int max) {
    char *endptr;
    errno = 0;
    long val = strtol(arg, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == arg || val < (long)min || val > (long)max) {
        fprintf(stderr, "Error: %s must be an integer between %d and %d (got: %s)\n",
                name, min, max, arg);
        exit(1);
    }
    return (int)val;
}

static int parse_ull_field(char **pos, unsigned long long *out) {
    char *start = *pos;
    errno = 0;
    *out = strtoull(start, pos, 10);
    if (*pos == start || errno != 0) return -1;
    return 0;
}

/* /sys/block/<dev>/stat lines are ~120 chars; 256 is generous */
#define STAT_BUF_SIZE 256

static int read_block_stats(int fd, BlockStats *stats) {
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) return -1;

    char buffer[STAT_BUF_SIZE];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) return -1;
    buffer[n] = '\0';

    char *pos = buffer;

    if (parse_ull_field(&pos, &stats->reads_completed) != 0 ||
        parse_ull_field(&pos, &stats->reads_merged) != 0 ||
        parse_ull_field(&pos, &stats->sectors_read) != 0 ||
        parse_ull_field(&pos, &stats->time_reading_ms) != 0 ||
        parse_ull_field(&pos, &stats->writes_completed) != 0 ||
        parse_ull_field(&pos, &stats->writes_merged) != 0 ||
        parse_ull_field(&pos, &stats->sectors_written) != 0 ||
        parse_ull_field(&pos, &stats->time_writing_ms) != 0 ||
        parse_ull_field(&pos, &stats->io_in_progress) != 0 ||
        parse_ull_field(&pos, &stats->time_io_ms) != 0 ||
        parse_ull_field(&pos, &stats->weighted_time_io_ms) != 0) {
        return -1;
    }

    return 0;
}

/* Resolve symlinks, verify block device, and find the correct /sys/block path */
static int resolve_device_paths(Config *cfg, const char *input_path) {
    char real_path[PATH_MAX];
    char *path_to_use;

    if (realpath(input_path, real_path)) {
        path_to_use = real_path;
    } else {
        path_to_use = (char *)input_path;
    }

    /* Verify the path is actually a block device */
    struct stat st;
    if (stat(path_to_use, &st) != 0 || !S_ISBLK(st.st_mode)) {
        return -1;
    }

    snprintf(cfg->device_path, sizeof(cfg->device_path), "%s", path_to_use);

    /* POSIX basename() may modify its argument, so use a copy */
    char path_copy[PATH_MAX];
    snprintf(path_copy, sizeof(path_copy), "%s", path_to_use);
    char *base = basename(path_copy);

    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", base);

    snprintf(cfg->sys_path, sizeof(cfg->sys_path), "/sys/block/%s/stat", cfg->device_name);

    if (access(cfg->sys_path, F_OK) == 0) {
        return 0;
    }

    /* Partition given (e.g. sdb1, nvme0n1p2) - resolve to parent block device */
    char parent[256];
    snprintf(parent, sizeof(parent), "%s", cfg->device_name);
    size_t len = strlen(parent);
    bool resolved = false;

    /* NVMe: nvme0n1p2 -> nvme0n1 (strip pN suffix) */
    char *last_p = strrchr(parent, 'p');
    if (last_p && last_p > parent && last_p[1] >= '0' && last_p[1] <= '9') {
        bool all_digits = true;
        for (const char *c = last_p + 1; *c; c++) {
            if (*c < '0' || *c > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            *last_p = '\0';
            resolved = true;
        }
    }

    /* Non-NVMe: strip trailing digits (sdb1 -> sdb) */
    if (!resolved) {
        len = strlen(parent);
        while (len > 0 && parent[len - 1] >= '0' && parent[len - 1] <= '9') {
            len--;
        }
        if (len > 0 && len < strlen(cfg->device_name)) {
            parent[len] = '\0';
            resolved = true;
        }
    }

    if (resolved) {
        snprintf(cfg->sys_path, sizeof(cfg->sys_path), "/sys/block/%s/stat", parent);
        if (access(cfg->sys_path, F_OK) == 0) {
            snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", parent);
            return 0;
        }
    }

    return -1;
}

static void print_progress_bar(const Config *cfg, int idle_count, int target_count, double read_mb, double write_mb) {
    if (cfg->quiet || cfg->verbose) return;

    const int width = 20;
    int progress = (target_count > 0) ? (idle_count * width) / target_count : 0;
    if (progress > width) progress = width;

    printf("\r%s[", cfg->no_color ? "" : COLOR_BLUE);
    for (int i = 0; i < width; i++) {
        if (i < progress) putchar('=');
        else if (i == progress) putchar('>');
        else putchar(' ');
    }
    printf("%s] ", cfg->no_color ? "" : COLOR_RESET);

    // Status Text
    if (idle_count == 0) {
        printf("%sACTIVITY%s ", cfg->no_color ? "" : COLOR_RED, cfg->no_color ? "" : COLOR_RESET);
    } else {
        printf("%sIDLE (%ds)%s  ", cfg->no_color ? "" : COLOR_GREEN, idle_count * cfg->poll_interval, cfg->no_color ? "" : COLOR_RESET);
    }

    // Stats
    printf("R: %6.1f MB/s  W: %6.1f MB/s   ", read_mb, write_mb);
    fflush(stdout);
}

static int monitor_loop(const Config *cfg) {
    int fd = open(cfg->sys_path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error opening %s: %s\n", cfg->sys_path, strerror(errno));
        return EXIT_ERROR;
    }

    BlockStats prev = {0};
    BlockStats curr = {0};

    if (read_block_stats(fd, &prev) != 0) {
        fprintf(stderr, "Error reading initial stats from %s: %s\n",
                cfg->sys_path, strerror(errno));
        close(fd);
        return EXIT_ERROR;
    }

    int required_periods = (cfg->idle_duration + cfg->poll_interval - 1) / cfg->poll_interval;
    int idle_count = 0;

    if (!cfg->quiet && !cfg->json_output) {
        printf("Monitoring %s%s%s (Target Idle: %ds)\n",
               cfg->no_color ? "" : COLOR_CYAN, cfg->device_path, cfg->no_color ? "" : COLOR_RESET,
               cfg->idle_duration);
    }

    while (keep_running) {
        sleep(cfg->poll_interval);
        if (!keep_running) break;

        if (read_block_stats(fd, &curr) != 0) {
            if (!cfg->json_output) {
                fputc('\n', stdout);
            }
            fprintf(stderr, "Error: Device read failed for %s: %s\n",
                    cfg->sys_path, strerror(errno));
            close(fd);
            return EXIT_ERROR;
        }

        unsigned long long sect_r = (curr.sectors_read >= prev.sectors_read) ?
                                    (curr.sectors_read - prev.sectors_read) : 0;
        unsigned long long sect_w = (curr.sectors_written >= prev.sectors_written) ?
                                    (curr.sectors_written - prev.sectors_written) : 0;

        long long io_time_diff = (long long)(curr.weighted_time_io_ms - prev.weighted_time_io_ms);
        if (io_time_diff < 0) io_time_diff = 0;

        double read_speed = (double)(sect_r * SECTOR_SIZE) / MB_DIVISOR / cfg->poll_interval;
        double write_speed = (double)(sect_w * SECTOR_SIZE) / MB_DIVISOR / cfg->poll_interval;

        bool is_active = (sect_r > 0) || (sect_w > 0) || (curr.io_in_progress > 0) || (io_time_diff > 0);

        if (is_active) {
            idle_count = 0;
        } else {
            idle_count++;
        }

        if (cfg->json_output) {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_buf[64];
            if (tm_info == NULL ||
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S%z", tm_info) == 0) {
                snprintf(time_buf, sizeof(time_buf), "%lld", (long long)now);
            }
            printf("{\"timestamp\":\"%s\",\"device\":\"%s\","
                   "\"read_mb_s\":%.2f,\"write_mb_s\":%.2f,"
                   "\"active\":%s,\"idle_seconds\":%d,"
                   "\"idle_target\":%d,\"io_in_progress\":%llu}\n",
                   time_buf, cfg->device_name,
                   read_speed, write_speed,
                   is_active ? "true" : "false",
                   idle_count * cfg->poll_interval,
                   cfg->idle_duration,
                   curr.io_in_progress);
            if (ferror(stdout)) {
                clearerr(stdout);
                close(fd);
                return EXIT_ERROR;
            }
        } else if (cfg->verbose) {
            printf("R: %.2f MB/s | W: %.2f MB/s | Active: %d | Idle: %d/%d\n",
                   read_speed, write_speed, is_active, idle_count, required_periods);
        } else {
            print_progress_bar(cfg, idle_count, required_periods, read_speed, write_speed);
        }

        if (idle_count >= required_periods) {
            if (cfg->json_output) {
                printf("{\"event\":\"idle\",\"device\":\"%s\",\"synced\":%s}\n",
                       cfg->device_name, cfg->auto_sync ? "true" : "false");
                if (ferror(stdout)) {
                    clearerr(stdout);
                    close(fd);
                    return EXIT_ERROR;
                }
            } else if (!cfg->quiet) {
                printf("\n\n%sDevice idle for %d seconds.%s\n",
                    cfg->no_color ? "" : COLOR_GREEN, cfg->idle_duration, cfg->no_color ? "" : COLOR_RESET);
            }

            if (cfg->auto_sync) {
                if (!cfg->quiet && !cfg->json_output) fputs("Syncing filesystems... ", stdout);
                fflush(stdout);
                sync();
                if (!cfg->quiet && !cfg->json_output) fputs("Done.\n", stdout);
            }

            if (!cfg->quiet && !cfg->json_output) {
                printf("%s✓ Safe to remove/unmount %s%s\n",
                    cfg->no_color ? "" : COLOR_GREEN, cfg->device_name, cfg->no_color ? "" : COLOR_RESET);
            }
            close(fd);
            return EXIT_SUCCESS_IDLE;
        }

        prev = curr;
    }

    close(fd);
    return EXIT_SIGNAL;
}

int main(int argc, char *argv[]) {
    /* Line-buffer stdout so JSON/verbose output reaches pipes (jq, log shippers)
     * immediately rather than waiting for the 4-8 KB fully-buffered chunk. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    Config cfg = {0};
    cfg.poll_interval = DEFAULT_POLL_INTERVAL;
    cfg.idle_duration = DEFAULT_IDLE_DURATION;
    cfg.auto_sync = true;

    struct option long_options[] = {
        {"interval", required_argument, 0, 'i'},
        {"idle-time", required_argument, 0, 't'},
        {"sync", no_argument, 0, 's'},
        {"no-sync", no_argument, 0, 'S'},
        {"quiet", no_argument, 0, 'q'},
        {"verbose", no_argument, 0, 'v'},
        {"json", no_argument, 0, 'j'},
        {"no-color", no_argument, 0, 'C'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:t:sjqvCShV", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': cfg.poll_interval = parse_int_arg(optarg, "interval",
                                                         MIN_POLL_INTERVAL, MAX_POLL_INTERVAL); break;
            case 't': cfg.idle_duration = parse_int_arg(optarg, "idle-time",
                                                         MIN_IDLE_DURATION, MAX_IDLE_DURATION); break;
            case 's': cfg.auto_sync = true; break;
            case 'S': cfg.auto_sync = false; break;
            case 'q': cfg.quiet = true; break;
            case 'v': cfg.verbose = true; break;
            case 'j': cfg.json_output = true; break;
            case 'C': cfg.no_color = true; break;
            case 'h': print_help(argv[0]); return 0;
            case 'V': printf("blk_monitor %s\n", VERSION); return 0;
            default: return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: Missing device path\n");
        return 1;
    }

    /* -i / -t bounds are enforced in parse_int_arg(); only the derived constraint
     * idle_duration >= poll_interval needs adjustment, and we tell the user. */
    if (cfg.idle_duration < cfg.poll_interval) {
        fprintf(stderr, "Notice: idle-time (%ds) is less than interval (%ds); raising to %ds.\n",
                cfg.idle_duration, cfg.poll_interval, cfg.poll_interval);
        cfg.idle_duration = cfg.poll_interval;
    }
    if (!isatty(STDOUT_FILENO) || getenv("NO_COLOR") != NULL) cfg.no_color = true;
    if (cfg.json_output) cfg.no_color = true;

    if (cfg.auto_sync && geteuid() != 0) {
        fprintf(stderr, "Warning: Running without root. sync() may not flush all device buffers.\n");
    }

    // Path setup
    if (resolve_device_paths(&cfg, argv[optind]) != 0) {
        fprintf(stderr, "Error: Could not access stats for %s: %s (Is it a block device?)\n",
                argv[optind], strerror(errno));
        return EXIT_ERROR;
    }

    setup_signals();
    atexit(cleanup_terminal);

    if (!cfg.quiet && !cfg.verbose && !cfg.json_output) {
        fputs(HIDE_CURSOR, stdout);
        cursor_hidden = true;
    }

    return monitor_loop(&cfg);
}
