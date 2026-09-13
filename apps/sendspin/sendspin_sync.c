#include <eaf/eaf_sendspin.h>
#include <limits.h>
#include <math.h>

#define EAF_SENDPIN_ADAPTIVE_FORGETTING_CUTOFF 3.0
#define EAF_SENDPIN_MAX_ERROR_SCALE 0.5
#define EAF_SENDPIN_DRIFT_SIGNIFICANCE_THRESHOLD_SQUARED 4.0
#define EAF_SENDPIN_FORGET_VARIANCE_FACTOR 4.0
#define EAF_SENDPIN_DRIFT_PROCESS_VARIANCE 1e-22

void eaf_sendspin_time_filter_reset(eaf_sendspin_time_filter_t *f) {
    f->count = 0;
    f->last_update = 0;
    f->offset = 0.0;
    f->drift = 0.0;
    f->offset_covariance = HUGE_VAL;
    f->offset_drift_covariance = 0.0;
    f->drift_covariance = 0.0;
    f->element = (eaf_sendspin_time_element_t){0};
}

void eaf_sendspin_time_filter_init(eaf_sendspin_time_filter_t *f) {
    f->process_variance = 0.0;
    f->drift_process_variance = EAF_SENDPIN_DRIFT_PROCESS_VARIANCE;
    f->forget_variance_factor = EAF_SENDPIN_FORGET_VARIANCE_FACTOR;
    eaf_sendspin_time_filter_reset(f);
}

void eaf_sendspin_time_filter_update(eaf_sendspin_time_filter_t *f, int64_t measurement,
                                     int64_t max_error, uint64_t time_added) {
    if (!f || time_added <= f->last_update)
        return;
    double dt = (double)(time_added - f->last_update);
    f->last_update = time_added;
    double update_std_dev = (double)max_error * EAF_SENDPIN_MAX_ERROR_SCALE;
    double measurement_variance = update_std_dev * update_std_dev;

    if (f->count == 0) {
        f->count = 1;
        f->offset = (double)measurement;
        f->offset_covariance = measurement_variance;
        f->drift = 0.0;
        f->element = (eaf_sendspin_time_element_t){time_added, f->offset, f->drift, false};
        return;
    }
    if (f->count == 1) {
        f->count = 2;
        f->drift = ((double)measurement - f->offset) / dt;
        f->offset = (double)measurement;
        f->drift_covariance = (f->offset_covariance + measurement_variance) / (dt * dt);
        f->offset_covariance = measurement_variance;
        f->element = (eaf_sendspin_time_element_t){time_added, f->offset, f->drift, false};
        return;
    }

    double offset = f->offset + f->drift * dt;
    double dt_squared = dt * dt;
    double drift_process_variance = dt * f->drift_process_variance;
    double new_drift_covariance = f->drift_covariance + drift_process_variance;
    double new_offset_drift_covariance = f->offset_drift_covariance + f->drift_covariance * dt;
    double offset_process_variance = dt * f->process_variance;
    double new_offset_covariance = f->offset_covariance + 2.0 * f->offset_drift_covariance * dt +
                                   f->drift_covariance * dt_squared + offset_process_variance;

    double residual = (double)measurement - offset;
    double max_residual_cutoff = (double)max_error * EAF_SENDPIN_ADAPTIVE_FORGETTING_CUTOFF;
    if (f->count < 100) {
        f->count += 1;
    } else if (fabs(residual) > max_residual_cutoff) {
        new_drift_covariance *= f->forget_variance_factor;
        new_offset_drift_covariance *= f->forget_variance_factor;
        new_offset_covariance *= f->forget_variance_factor;
    }

    double uncertainty = 1.0 / (new_offset_covariance + measurement_variance);
    double offset_gain = new_offset_covariance * uncertainty;
    double drift_gain = new_offset_drift_covariance * uncertainty;
    f->offset = offset + offset_gain * residual;
    f->drift += drift_gain * residual;
    f->drift_covariance = new_drift_covariance - drift_gain * new_offset_drift_covariance;
    f->offset_drift_covariance = new_offset_drift_covariance - drift_gain * new_offset_covariance;
    f->offset_covariance = new_offset_covariance - offset_gain * new_offset_covariance;

    bool use_drift = f->drift * f->drift >
                     EAF_SENDPIN_DRIFT_SIGNIFICANCE_THRESHOLD_SQUARED * f->drift_covariance;
    f->element = (eaf_sendspin_time_element_t){time_added, f->offset, f->drift, use_drift};
}

void eaf_sendspin_time_filter_exchange(eaf_sendspin_time_filter_t *f, int64_t client_transmitted,
                                       int64_t server_received, int64_t server_transmitted,
                                       uint64_t client_received) {
    if (!f)
        return;
    int64_t received = (int64_t)client_received;
    int64_t measurement =
        ((server_received - client_transmitted) + (server_transmitted - received)) / 2;
    int64_t max_error =
        ((received - client_transmitted) - (server_transmitted - server_received)) / 2;
    eaf_sendspin_time_filter_update(f, measurement, max_error, client_received);
}

int64_t eaf_sendspin_compute_server_time(const eaf_sendspin_time_filter_t *f, int64_t client_time) {
    if (!f)
        return client_time;
    double effective_drift = f->element.use_drift ? f->element.drift : 0.0;
    double dt = (double)(client_time - (int64_t)f->element.last_update);
    double offset = round(f->element.offset + effective_drift * dt);
    return client_time + (int64_t)offset;
}

int64_t eaf_sendspin_compute_client_time(const eaf_sendspin_time_filter_t *f, int64_t server_time) {
    if (!f)
        return server_time;
    double effective_drift = f->element.use_drift ? f->element.drift : 0.0;
    double numerator = (double)server_time - f->element.offset +
                       effective_drift * (double)(int64_t)f->element.last_update;
    return (int64_t)round(numerator / (1.0 + effective_drift));
}

bool eaf_sendspin_time_synchronized(const eaf_sendspin_time_filter_t *f) {
    return f && f->count >= 2 && !isinf(f->offset_covariance);
}

int64_t eaf_sendspin_time_error(const eaf_sendspin_time_filter_t *f) {
    if (!f || !isfinite(f->offset_covariance) || f->offset_covariance < 0.0)
        return INT64_MAX;
    return (int64_t)round(sqrt(f->offset_covariance));
}
