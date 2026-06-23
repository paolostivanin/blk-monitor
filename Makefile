CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O3
LDFLAGS ?=

COMMON_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wformat=2 \
	-Wmissing-format-attribute -Wstrict-prototypes -Wundef \
	-Werror=format-security -Werror=implicit-function-declaration
HARDEN_CFLAGS = -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
	-fstack-protector-strong -fPIE
HARDEN_LDFLAGS = -pie -Wl,-z,relro,-z,now
DEBUG_CFLAGS = -O0 -g
SANITIZER_CFLAGS = -O1 -g -fno-omit-frame-pointer \
	-fsanitize=address,undefined
SANITIZER_LDFLAGS = -fsanitize=address,undefined

TARGET = blk_monitor
DEBUG_TARGET = blk_monitor-debug
ASAN_TARGET = blk_monitor-asan
UNIT_TARGET = tests/unit
UNIT_ASAN_TARGET = tests/unit-asan
SOURCES = blk_monitor.c blk_monitor_core.c
HEADERS = blk_monitor_core.h
MANPAGE = blk_monitor.1

PREFIX ?= /usr/local
BINDIR = $(DESTDIR)$(PREFIX)/bin
MANDIR = $(DESTDIR)$(PREFIX)/share/man/man1

.PHONY: all release debug asan check check-asan check-e2e lint clean \
	install uninstall

all: release

release: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) $(HARDEN_CFLAGS) \
		$(LDFLAGS) $(HARDEN_LDFLAGS) -o $@ $(SOURCES)

debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(COMMON_CFLAGS) -fPIE \
		$(LDFLAGS) -pie -o $@ $(SOURCES)

asan: $(ASAN_TARGET)

$(ASAN_TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(SANITIZER_CFLAGS) $(COMMON_CFLAGS) -fPIE \
		$(LDFLAGS) $(SANITIZER_LDFLAGS) -pie -o $@ $(SOURCES)

$(UNIT_TARGET): tests/unit.c blk_monitor_core.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON_CFLAGS) -I. \
		$(LDFLAGS) -o $@ tests/unit.c blk_monitor_core.c

$(UNIT_ASAN_TARGET): tests/unit.c blk_monitor_core.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(SANITIZER_CFLAGS) $(COMMON_CFLAGS) -I. \
		$(LDFLAGS) $(SANITIZER_LDFLAGS) -o $@ \
		tests/unit.c blk_monitor_core.c

check: $(TARGET) $(UNIT_TARGET)
	./$(UNIT_TARGET)
	BIN=./$(TARGET) ./tests/smoke.sh

check-asan: $(ASAN_TARGET) $(UNIT_ASAN_TARGET)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		./$(UNIT_ASAN_TARGET)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
		BIN=./$(ASAN_TARGET) SKIP_LOOPBACK=1 ./tests/smoke.sh

check-e2e: $(TARGET)
	BIN=./$(TARGET) ./tests/smoke.sh

lint:
	shellcheck tests/smoke.sh
	clang-tidy -checks='clang-analyzer-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,bugprone-*,-bugprone-reserved-identifier,-bugprone-easily-swappable-parameters' \
		blk_monitor.c blk_monitor_core.c -- -std=c11 -I.

clean:
	rm -f $(TARGET) $(DEBUG_TARGET) $(ASAN_TARGET) $(UNIT_TARGET) \
		$(UNIT_ASAN_TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/
	install -d $(MANDIR)
	install -m 644 $(MANPAGE) $(MANDIR)/

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(MANDIR)/$(MANPAGE)
