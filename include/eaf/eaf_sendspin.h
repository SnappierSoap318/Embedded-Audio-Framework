#pragma once
#include <eaf/eaf_types.h>

/* Sendspin client protocol (captured MA 2.10.3 cleartext revision).
   Storage is caller-owned; no dynamic allocation and no OS headers. */

/* --- Time filter -----------------------------------------------------------
   Two-dimensional Kalman filter tracking server/client offset and clock drift.
   Port of the reference aiosendspin implementation. All timestamps are
   microseconds. See docs/bench/sendspin-capture-2026-09-13.md. */

typedef struct {
    uint64_t last_update;
    double offset;
    double drift;
    bool use_drift;
} eaf_sendspin_time_element_t;

typedef struct {
    uint32_t count;
    uint64_t last_update;
    double offset, drift;
    double offset_covariance, offset_drift_covariance, drift_covariance;
    double process_variance, drift_process_variance, forget_variance_factor;
    eaf_sendspin_time_element_t element;
} eaf_sendspin_time_filter_t;

void eaf_sendspin_time_filter_init(eaf_sendspin_time_filter_t *f);
void eaf_sendspin_time_filter_reset(eaf_sendspin_time_filter_t *f);
/* measurement = ((server_received - client_transmitted) +
                  (server_transmitted - client_received)) / 2
   max_error   = ((client_received - client_transmitted) -
                  (server_transmitted - server_received)) / 2
   Computes the NTP-style offset/uncertainty and feeds the filter. */
void eaf_sendspin_time_filter_exchange(eaf_sendspin_time_filter_t *f, int64_t client_transmitted,
                                       int64_t server_received, int64_t server_transmitted,
                                       uint64_t client_received);
/* Raw update; time_added must be monotonic or the sample is ignored. */
void eaf_sendspin_time_filter_update(eaf_sendspin_time_filter_t *f, int64_t measurement,
                                     int64_t max_error, uint64_t time_added);
int64_t eaf_sendspin_compute_server_time(const eaf_sendspin_time_filter_t *f, int64_t client_time);
int64_t eaf_sendspin_compute_client_time(const eaf_sendspin_time_filter_t *f, int64_t server_time);
bool eaf_sendspin_time_synchronized(const eaf_sendspin_time_filter_t *f);
int64_t eaf_sendspin_time_error(const eaf_sendspin_time_filter_t *f);
