CC = gcc
CFLAGS = -Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong -Wundef -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code -Wchar-subscripts -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fPIE
LDFLAGS = -pie -Wl,-z,relro,-z,now
TARGET = blk_monitor
MANPAGE = blk_monitor.1

PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man1

.PHONY: all clean install uninstall debug asan check

all: $(TARGET)

$(TARGET): blk_monitor.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) blk_monitor.c

# Compile with debug symbols
debug: CFLAGS = -O0 -g -Wall -Wextra -Wpedantic -std=c11 -fPIE
debug: LDFLAGS = -pie
debug: $(TARGET)

# Compile with AddressSanitizer + UndefinedBehaviorSanitizer for development.
# Strips the hardening flags that conflict with ASan instrumentation.
asan: CFLAGS = -O1 -g -Wall -Wextra -fno-omit-frame-pointer -fsanitize=address,undefined -fPIE
asan: LDFLAGS = -fsanitize=address,undefined -pie
asan: $(TARGET)

check: $(TARGET)
	./tests/smoke.sh

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/
	install -d $(MANDIR)
	install -m 644 $(MANPAGE) $(MANDIR)/

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(MANDIR)/$(MANPAGE)
