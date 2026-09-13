#pragma once
#include <stddef.h>
/* Bounded application event history; concurrent writers drop rather than wait. */
void board_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* Same history, without synchronous UART output; use for periodic telemetry. */
void board_log_memory(const char *format, ...) __attribute__((format(printf, 1, 2)));
size_t board_log_read(char *buffer, size_t capacity);
void board_diagnostics_start(void);
