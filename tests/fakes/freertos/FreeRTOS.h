#pragma once
/* Minimal host shim: the binding only needs a critical section that provides
 * memory ordering across the callback and worker tasks. Single-threaded tests
 * compile the macros away. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
