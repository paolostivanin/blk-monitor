CC = gcc
CFLAGS = -Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong -Wundef -Wmissing-format-attribute -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code -Wchar-subscripts -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
TARGET = blk_monitor

.PHONY: all clean install

all: $(TARGET)

$(TARGET): blk_monitor.c
	$(CC) $(CFLAGS) -o $(TARGET) blk_monitor.c

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Compile with debug symbols
debug: CFLAGS = -O0 -g -Wall -Wextra -Wpedantic -std=c11
debug: $(TARGET)
