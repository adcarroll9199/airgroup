#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#define LOG_RING_BYTES 4096

static char ring[LOG_RING_BYTES];
static size_t head;   // next write position
static bool wrapped;

void log_printf(const char *fmt, ...) {
    char line[256];
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    int n = snprintf(line, sizeof line, "%5lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    size_t len = strlen(line);
    line[len++] = '\n';
    line[len] = 0;
    fputs(line, stdout);

    for (size_t i = 0; i < len; i++) {
        ring[head++] = line[i];
        if (head == LOG_RING_BYTES) {
            head = 0;
            wrapped = true;
        }
    }
}

size_t log_snapshot(char *buf, size_t len) {
    size_t n = 0;
    if (wrapped) {
        size_t tail = LOG_RING_BYTES - head;
        n = tail < len ? tail : len;
        memcpy(buf, ring + head, n);
    }
    size_t m = head < len - n ? head : len - n;
    memcpy(buf + n, ring, m);
    return n + m;
}
