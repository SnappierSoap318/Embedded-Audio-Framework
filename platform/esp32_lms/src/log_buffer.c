#include "diagnostics.h"
#include <eaf/eaf_hal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define LOG_LINES 24u
#define LOG_WIDTH 160u
static char lines[LOG_LINES][LOG_WIDTH];
static size_t next, count;
static atomic_flag lock = ATOMIC_FLAG_INIT;
static atomic_uint dropped;
void board_log(const char *format, ...) {
    char line[LOG_WIDTH] = {0};
    int prefix = snprintf(line, sizeof(line), "[%llu ms] ",
                          (unsigned long long)(hal_monotonic_time_us() / 1000u));
    if (prefix < 0 || (size_t)prefix >= sizeof(line))
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
    va_end(args);
    if (atomic_flag_test_and_set(&lock)) {
        atomic_fetch_add(&dropped, 1u);
        return;
    }
    memcpy(lines[next], line, sizeof(line));
    next = (next + 1u) % LOG_LINES;
    if (count < LOG_LINES)
        ++count;
    atomic_flag_clear(&lock);
    /* UART remains useful before Wi-Fi comes up. No logging in the hot path. */
    printf("%s\n", line);
}
size_t board_log_read(char *buffer, size_t capacity) {
    if (!buffer || !capacity)
        return 0;
    int n =
        snprintf(buffer, capacity, "Application diagnostics; dropped=%u\n", atomic_load(&dropped));
    if (n < 0)
        return 0;
    size_t used = (size_t)n < capacity ? (size_t)n : capacity - 1u;
    if (atomic_flag_test_and_set(&lock))
        return used;
    for (size_t i = 0; i < count; ++i) {
        const char *line = lines[(next + LOG_LINES - count + i) % LOG_LINES];
        size_t length = strlen(line);
        if (length + 1u >= capacity - used)
            break;
        memcpy(buffer + used, line, length);
        used += length;
        buffer[used++] = '\n';
    }
    buffer[used] = '\0';
    atomic_flag_clear(&lock);
    return used;
}
