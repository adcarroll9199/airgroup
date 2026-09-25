#pragma once
#include <stddef.h>

// printf to USB serial, and keep the recent lines in RAM so they can be read over Wi-Fi at /log.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Copies the retained log (oldest first) into buf; returns bytes written.
size_t log_snapshot(char *buf, size_t len);

#define LOG(...) log_printf(__VA_ARGS__)
