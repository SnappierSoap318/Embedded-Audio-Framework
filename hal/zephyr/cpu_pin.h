#pragma once
#include <stdbool.h>
#include <zephyr/kernel.h>

/* CPU affinity is a kernel feature (CONFIG_SCHED_CPU_MASK). When it is not
   built, k_thread_cpu_pin is undeclared, so the implementation is chosen at
   build time instead of with a preprocessor branch. */

/* True when the kernel exposes CPU pinning. */
bool hal_zephyr_cpu_pin_available(void);

/* Pins a kernel thread; returns 0 or a negative kernel error. */
int hal_zephyr_cpu_pin(k_tid_t thread, int cpu);
