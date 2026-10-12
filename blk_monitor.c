/*
 * blk_monitor.c - Lightweight Linux block-device I/O monitor
 */

#define _GNU_SOURCE

#include "blk_monitor_core.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_POLL_INTERVAL 2
#define DEFAULT_IDLE_DURATION 10
#define MIN_POLL_INTERVAL 1
#define MAX_POLL_INTERVAL 60
#define MIN_IDLE_DURATION 5
#define MAX_IDLE_DURATION 3600

#define SECTOR_SIZE 512ULL
#define MB_DIVISOR (1024.0 * 1024.0)
#define STAT_BUF_SIZE 1024
#define MIN_ELAPSED_SECONDS 0.001

#ifndef VERSION
#define VERSION "unknown" /* normally supplied by the Makefile */
#endif

#define EXIT_SUCCESS_IDLE 0
#define EXIT_ERROR 1
#define EXIT_SIGNAL 2

#define COLOR_RESET "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_BLUE "\033[34m"
#define COLOR_CYAN "\033[36m"
#define HIDE_CURSOR "\033[?25l"
#define SHOW_CURSOR "\033[?25h"

typedef struct {
    char device_path[PATH_MAX];
    char sys_path[PATH_MAX];
    char device_name[256];
    int poll_interval;
    int idle_duration;
    bool auto_sync;
    bool quiet;
    bool verbose;
    bool no_color;
    bool json_output;
    bool interactive;
} Config;

static volatile sig_atomic_t keep_running = 1;
static bool cursor_hidden = false;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 0)))
#endif
static int out_vprintf(const char *format, va_list args) {
    if (vfprintf(stdout, format, args) < 0) {
        return -1;
    }
    return 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static int out_printf(const char *format, ...) {
    int result;
    va_list args;

    va_start(args, format);
    result = out_vprintf(format, args);
    va_end(args);
    return result;
}

static int out_puts(const char *text) {
    return fputs(text, stdout) == EOF ? -1 : 0;
}

static int out_flush(void) {
    return fflush(stdout) == EOF ? -1 : 0;
}

static void handle_signal(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void setup_signals(sigset_t *wait_mask) {
    struct sigaction action = {0};
    struct sigaction ignore = {0};
    sigset_t block;

    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "Warning: failed to set SIGINT handler: %s\n",
                strerror(errno));
    }
    if (sigaction(SIGTERM, &action, NULL) != 0) {
        fprintf(stderr, "Warning: failed to set SIGTERM handler: %s\n",
                strerror(errno));
    }

    /*
     * Keep SIGINT/SIGTERM blocked outside wait_interval() so delivery is
     * race-free; wait_mask is the original mask used while waiting.
     */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &block, wait_mask) != 0) {
        fprintf(stderr, "Warning: failed to block signals: %s\n",
                strerror(errno));
        (void)sigprocmask(SIG_SETMASK, NULL, wait_mask);
    }

    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) != 0) {
        fprintf(stderr, "Warning: failed to ignore SIGPIPE: %s\n",
                strerror(errno));
    }
}

static void cleanup_terminal(void) {
    if (cursor_hidden && isatty(STDOUT_FILENO)) {
        (void)fputs(SHOW_CURSOR COLOR_RESET "\n", stdout);
        (void)fflush(stdout);
    }
    cursor_hidden = false;
}

static int print_help(const char *program_name) {
    return out_printf(
        "USAGE: %s [OPTIONS] /dev/DEVICE\n"
        "Monitor block-device I/O and exit after a consecutive idle period.\n"
        "This does not unmount or eject the device.\n\n"
        "  -i, --interval SEC   Poll interval in seconds (%d-%d, default: %d)\n"
        "  -t, --idle-time SEC  Required idle time in seconds (%d-%d, default: %d)\n"
        "  -s, --sync           Run system-wide sync after idle (default)\n"
        "  -S, --no-sync        Do not run sync after idle\n"
        "  -q, --quiet          Print only the final status line\n"
        "  -v, --verbose        Print one status line per poll\n"
        "  -j, --json           Print one JSON object per line\n"
        "  -C, --no-color       Disable colors\n"
        "  -h, --help           Show this help\n"
        "  -V, --version        Show version\n",
        program_name, MIN_POLL_INTERVAL, MAX_POLL_INTERVAL,
        DEFAULT_POLL_INTERVAL, MIN_IDLE_DURATION, MAX_IDLE_DURATION,
        DEFAULT_IDLE_DURATION);
}

static int parse_int_arg(const char *argument, const char *name, int minimum,
                         int maximum, int *value) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(argument, &end, 10);
    if (errno != 0 || end == argument || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        fprintf(stderr,
                "Error: %s must be an integer between %d and %d (got: %s)\n",
                name, minimum, maximum, argument);
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int copy_string(char *destination, size_t destination_size,
                       const char *source) {
    int written = snprintf(destination, destination_size, "%s", source);
    if (written < 0 || (size_t)written >= destination_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static DeviceResolveResult resolve_device_paths(Config *config,
                                                const char *input_path) {
    char resolved_device[PATH_MAX];
    struct stat status;

    if (realpath(input_path, resolved_device) == NULL) {
        return errno == ENAMETOOLONG ? DEVICE_RESOLVE_PATH_TOO_LONG
                                    : DEVICE_RESOLVE_NOT_FOUND;
    }
    if (stat(resolved_device, &status) != 0) {
        return DEVICE_RESOLVE_NOT_FOUND;
    }
    if (!S_ISBLK(status.st_mode)) {
        return DEVICE_RESOLVE_NOT_BLOCK;
    }
    if (copy_string(config->device_path, sizeof(config->device_path),
                    resolved_device) != 0) {
        return DEVICE_RESOLVE_PATH_TOO_LONG;
    }
    return blk_sysfs_resolve("/sys", path_basename(resolved_device),
                             config->device_name, sizeof(config->device_name),
                             config->sys_path, sizeof(config->sys_path));
}

static void print_device_error(DeviceResolveResult result,
                               const char *input_path) {
    switch (result) {
        case DEVICE_RESOLVE_NOT_FOUND:
            fprintf(stderr, "Error: device path does not exist: %s\n", input_path);
            break;
        case DEVICE_RESOLVE_NOT_BLOCK:
            fprintf(stderr, "Error: path is not a block device: %s\n", input_path);
            break;
        case DEVICE_RESOLVE_PATH_TOO_LONG:
            fprintf(stderr, "Error: device path is too long: %s\n", input_path);
            break;
        case DEVICE_RESOLVE_SYSFS_UNAVAILABLE:
            fprintf(stderr,
                    "Error: readable block statistics are unavailable for %s\n",
                    input_path);
            break;
        case DEVICE_RESOLVE_OK:
            break;
    }
}

static int read_block_stats(int file_descriptor, BlockStats *stats) {
    char buffer[STAT_BUF_SIZE];
    ssize_t length;

    if (lseek(file_descriptor, 0, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    length = read(file_descriptor, buffer, sizeof(buffer) - 1);
    if (length < 0) {
        return -1;
    }
    if (length == 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)length == sizeof(buffer) - 1) {
        errno = EOVERFLOW;
        return -1;
    }
    buffer[length] = '\0';
    return block_stats_parse(buffer, stats);
}

static int print_progress_bar(const Config *config, int idle_seconds,
                              double read_mb, double write_mb) {
    const int width = 20;
    int progress = (idle_seconds * width) / config->idle_duration;

    if (progress > width) {
        progress = width;
    }
    if (out_printf("\r%s[", config->no_color ? "" : COLOR_BLUE) != 0) {
        return -1;
    }
    for (int index = 0; index < width; index++) {
        int marker = index < progress ? '=' : (index == progress ? '>' : ' ');
        if (fputc(marker, stdout) == EOF) {
            return -1;
        }
    }
    if (out_printf("%s] ", config->no_color ? "" : COLOR_RESET) != 0) {
        return -1;
    }
    if (idle_seconds == 0) {
        if (out_printf("%sACTIVITY%s ", config->no_color ? "" : COLOR_RED,
                       config->no_color ? "" : COLOR_RESET) != 0) {
            return -1;
        }
    } else if (out_printf("%sIDLE (%ds)%s  ",
                          config->no_color ? "" : COLOR_GREEN, idle_seconds,
                          config->no_color ? "" : COLOR_RESET) != 0) {
        return -1;
    }
    if (out_printf("R: %6.1f MB/s  W: %6.1f MB/s   ", read_mb, write_mb) != 0) {
        return -1;
    }
    return out_flush();
}

static int print_sample(const Config *config, const BlockStats *current,
                        const BlockDelta *delta, int idle_seconds,
                        double read_speed, double write_speed) {
    if (config->json_output) {
        time_t now = time(NULL);
        struct tm local_time;
        char timestamp[64];

        if (localtime_r(&now, &local_time) == NULL ||
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z",
                     &local_time) == 0) {
            if (snprintf(timestamp, sizeof(timestamp), "%lld",
                         (long long)now) < 0) {
                return -1;
            }
        }
        return out_printf(
            "{\"timestamp\":\"%s\",\"device\":\"%s\","
            "\"read_mb_s\":%.2f,\"write_mb_s\":%.2f,"
            "\"active\":%s,\"idle_seconds\":%d,\"idle_target\":%d,"
            "\"io_in_progress\":%llu,\"counters_reset\":%s}\n",
            timestamp, config->device_name, read_speed, write_speed,
            delta->active ? "true" : "false", idle_seconds,
            config->idle_duration, current->io_in_progress,
            delta->counters_reset ? "true" : "false");
    }
    if (config->verbose || !config->interactive) {
        return out_printf(
            "R: %.2f MB/s | W: %.2f MB/s | Active: %d | Idle: %d/%d%s\n",
            read_speed, write_speed, delta->active, idle_seconds,
            config->idle_duration,
            delta->counters_reset ? " | Counters reset" : "");
    }
    return print_progress_bar(config, idle_seconds, read_speed, write_speed);
}

static int print_final_status(const Config *config, int idle_seconds) {
    const char *sync_text =
        config->auto_sync ? "system-wide sync completed"
                          : "system-wide sync skipped";

    if (config->json_output) {
        return out_printf(
            "{\"event\":\"idle\",\"device\":\"%s\",\"idle_seconds\":%d,"
            "\"sync_performed\":%s,\"sync_scope\":\"system\","
            "\"unmount_performed\":false}\n",
            config->device_name, idle_seconds,
            config->auto_sync ? "true" : "false");
    }

    if (config->quiet) {
        return out_printf(
            "I/O idle for %d seconds on %s; %s. Device remains mounted; "
            "unmount/eject before removal.\n",
            idle_seconds, config->device_name, sync_text);
    }

    if (config->interactive && out_puts("\n") != 0) {
        return -1;
    }
    return out_printf(
        "%sI/O idle for %d seconds on %s; %s.%s\n"
        "Device remains mounted; unmount/eject before physical removal.\n",
        config->no_color ? "" : COLOR_GREEN, idle_seconds,
        config->device_name, sync_text,
        config->no_color ? "" : COLOR_RESET);
}

static double elapsed_seconds(const struct timespec *from,
                              const struct timespec *to) {
    return (double)(to->tv_sec - from->tv_sec) +
           (double)(to->tv_nsec - from->tv_nsec) / 1e9;
}

/*
 * Sleep for the poll interval with SIGINT/SIGTERM unblocked only for the
 * duration of the wait, so a signal can never slip in between the
 * keep_running check and the sleep.
 */
static void wait_timeout(const struct timespec *timeout,
                         const sigset_t *wait_mask) {
    (void)ppoll(NULL, 0, timeout, wait_mask);
}

static void wait_interval(int seconds, const sigset_t *wait_mask) {
    struct timespec timeout = {.tv_sec = seconds, .tv_nsec = 0};

    wait_timeout(&timeout, wait_mask);
}

static void report_stats_error(const Config *config, const char *what) {
    switch (errno) {
        case ENODEV:
        case ENOENT:
        case ENXIO:
        case EIO:
            fprintf(stderr, "Error: %s is no longer available (%s): %s\n",
                    config->device_name, what, strerror(errno));
            break;
        default:
            fprintf(stderr, "Error: cannot read %s statistics from %s: %s\n",
                    what, config->sys_path, strerror(errno));
            break;
    }
}

/*
 * sync() runs in a child so a hung device cannot make SIGINT/SIGTERM
 * ineffective. Returns 0 when done, 1 if interrupted by a signal, -1 on error.
 */
static int run_sync(const Config *config, const sigset_t *wait_mask) {
    const struct timespec poll_delay = {.tv_sec = 0, .tv_nsec = 100000000L};
    bool announce = !config->quiet && !config->json_output;
    pid_t child;

    if (announce &&
        out_puts(config->interactive
                     ? "\nSyncing all mounted filesystems... "
                     : "Syncing all mounted filesystems...\n") != 0) {
        return -1;
    }
    if (out_flush() != 0) {
        return -1;
    }

    child = fork();
    if (child < 0) {
        sync(); /* cannot fork: fall back to an uninterruptible sync */
    } else if (child == 0) {
        sync();
        _exit(0);
    } else {
        for (;;) {
            int status;
            pid_t waited = waitpid(child, &status, WNOHANG);

            if (waited == child) {
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    fprintf(stderr, "Error: sync process failed\n");
                    return -1;
                }
                break;
            }
            if (waited < 0 && errno != EINTR) {
                fprintf(stderr, "Error: cannot wait for sync: %s\n",
                        strerror(errno));
                return -1;
            }
            if (!keep_running) {
                (void)kill(child, SIGKILL);
                return 1;
            }
            wait_timeout(&poll_delay, wait_mask);
        }
    }

    if (announce && config->interactive && out_puts("Done.\n") != 0) {
        return -1;
    }
    return 0;
}

static int monitor_loop(const Config *config, const sigset_t *wait_mask) {
    int file_descriptor = open(config->sys_path, O_RDONLY | O_CLOEXEC);
    BlockStats previous = {0};
    struct timespec previous_time;
    double idle_elapsed = 0.0;
    int result = EXIT_SIGNAL;

    if (file_descriptor < 0) {
        fprintf(stderr, "Error: cannot open %s: %s\n", config->sys_path,
                strerror(errno));
        return EXIT_ERROR;
    }
    if (read_block_stats(file_descriptor, &previous) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &previous_time) != 0) {
        report_stats_error(config, "initial");
        result = EXIT_ERROR;
        goto out;
    }

    if (!config->quiet && !config->json_output &&
        out_printf("Monitoring %s%s%s (idle target: %ds)\n",
                   config->no_color ? "" : COLOR_CYAN, config->device_path,
                   config->no_color ? "" : COLOR_RESET,
                   config->idle_duration) != 0) {
        result = EXIT_ERROR;
        goto out;
    }

    while (keep_running) {
        BlockStats current = {0};
        BlockDelta delta;
        struct timespec current_time;
        double elapsed;
        double read_speed;
        double write_speed;
        int idle_seconds;

        wait_interval(config->poll_interval, wait_mask);
        if (!keep_running) {
            break;
        }
        if (read_block_stats(file_descriptor, &current) != 0 ||
            clock_gettime(CLOCK_MONOTONIC, &current_time) != 0) {
            report_stats_error(config, "current");
            result = EXIT_ERROR;
            goto out;
        }

        /* Use measured time: the wait may overrun (load, SIGSTOP/SIGCONT). */
        elapsed = elapsed_seconds(&previous_time, &current_time);
        if (elapsed < MIN_ELAPSED_SECONDS) {
            elapsed = MIN_ELAPSED_SECONDS;
        }

        delta = block_stats_delta(&previous, &current);
        read_speed = (double)delta.sectors_read * (double)SECTOR_SIZE /
                     MB_DIVISOR / elapsed;
        write_speed = (double)delta.sectors_written * (double)SECTOR_SIZE /
                      MB_DIVISOR / elapsed;
        idle_elapsed = delta.active ? 0.0 : idle_elapsed + elapsed;
        idle_seconds = (int)(idle_elapsed + 0.5);

        if (!config->quiet &&
            print_sample(config, &current, &delta, idle_seconds,
                         read_speed, write_speed) != 0) {
            result = EXIT_ERROR;
            goto out;
        }

        if (idle_seconds >= config->idle_duration) {
            int sync_result = config->auto_sync ? run_sync(config, wait_mask) : 0;

            if (sync_result > 0) {
                result = EXIT_SIGNAL;
            } else if (sync_result < 0 ||
                       print_final_status(config, idle_seconds) != 0 ||
                       out_flush() != 0) {
                result = EXIT_ERROR;
            } else {
                result = EXIT_SUCCESS_IDLE;
            }
            goto out;
        }
        previous = current;
        previous_time = current_time;
    }

out:
    (void)close(file_descriptor);
    return result;
}

int main(int argc, char *argv[]) {
    Config config = {0};
    DeviceResolveResult resolve_result;
    sigset_t wait_mask;
    int option;

    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "Error: cannot configure stdout buffering\n");
        return EXIT_ERROR;
    }

    config.poll_interval = DEFAULT_POLL_INTERVAL;
    config.idle_duration = DEFAULT_IDLE_DURATION;
    config.auto_sync = true;
    config.interactive = isatty(STDOUT_FILENO);

    static const struct option long_options[] = {
        {"interval", required_argument, NULL, 'i'},
        {"idle-time", required_argument, NULL, 't'},
        {"sync", no_argument, NULL, 's'},
        {"no-sync", no_argument, NULL, 'S'},
        {"quiet", no_argument, NULL, 'q'},
        {"verbose", no_argument, NULL, 'v'},
        {"json", no_argument, NULL, 'j'},
        {"no-color", no_argument, NULL, 'C'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

    while ((option = getopt_long(argc, argv, "i:t:sjqvCShV", long_options,
                                 NULL)) != -1) {
        switch (option) {
            case 'i':
                if (parse_int_arg(optarg, "interval", MIN_POLL_INTERVAL,
                                  MAX_POLL_INTERVAL,
                                  &config.poll_interval) != 0) {
                    return EXIT_ERROR;
                }
                break;
            case 't':
                if (parse_int_arg(optarg, "idle-time", MIN_IDLE_DURATION,
                                  MAX_IDLE_DURATION,
                                  &config.idle_duration) != 0) {
                    return EXIT_ERROR;
                }
                break;
            case 's':
                config.auto_sync = true;
                break;
            case 'S':
                config.auto_sync = false;
                break;
            case 'q':
                config.quiet = true;
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'j':
                config.json_output = true;
                break;
            case 'C':
                config.no_color = true;
                break;
            case 'h':
                return print_help(argv[0]) == 0 && out_flush() == 0
                           ? EXIT_SUCCESS_IDLE : EXIT_ERROR;
            case 'V':
                return out_printf("blk_monitor %s\n", VERSION) == 0 &&
                               out_flush() == 0
                           ? EXIT_SUCCESS_IDLE : EXIT_ERROR;
            default:
                return EXIT_ERROR;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: missing device path\n");
        return EXIT_ERROR;
    }
    if (optind + 1 != argc) {
        fprintf(stderr, "Error: unexpected extra operand: %s\n",
                argv[optind + 1]);
        return EXIT_ERROR;
    }
    if (config.quiet && config.verbose) {
        fprintf(stderr, "Error: --quiet and --verbose are mutually exclusive\n");
        return EXIT_ERROR;
    }
    if (config.json_output && config.verbose) {
        fprintf(stderr, "Error: --json and --verbose are mutually exclusive\n");
        return EXIT_ERROR;
    }
    if (config.idle_duration < config.poll_interval) {
        fprintf(stderr,
                "Notice: idle-time (%ds) is less than interval (%ds); "
                "raising it to %ds.\n",
                config.idle_duration, config.poll_interval,
                config.poll_interval);
        config.idle_duration = config.poll_interval;
    }
    if (!config.interactive || getenv("NO_COLOR") != NULL ||
        config.json_output) {
        config.no_color = true;
    }

    resolve_result = resolve_device_paths(&config, argv[optind]);
    if (resolve_result != DEVICE_RESOLVE_OK) {
        print_device_error(resolve_result, argv[optind]);
        return EXIT_ERROR;
    }

    setup_signals(&wait_mask);
    if (atexit(cleanup_terminal) != 0) {
        fprintf(stderr, "Error: cannot register terminal cleanup\n");
        return EXIT_ERROR;
    }
    if (config.interactive && !config.quiet && !config.verbose &&
        !config.json_output) {
        if (out_puts(HIDE_CURSOR) != 0 || out_flush() != 0) {
            return EXIT_ERROR;
        }
        cursor_hidden = true;
    }

    return monitor_loop(&config, &wait_mask);
}
