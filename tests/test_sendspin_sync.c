#include "check.h"
#include <eaf/eaf_sendspin.h>
#include <math.h>
#include <stdint.h>

static int64_t server_time(int64_t client_us, int64_t offset_us, double drift) {
    return (int64_t)llround((double)client_us * (1.0 + drift) + (double)offset_us);
}

static void feed(eaf_sendspin_time_filter_t *f, int64_t client_us, int64_t delay_us,
                 int64_t offset_us, double drift) {
    int64_t transmitted = client_us;
    int64_t received = client_us + 2 * delay_us;
    int64_t server_received = server_time(client_us + delay_us, offset_us, drift);
    eaf_sendspin_time_filter_exchange(f, transmitted, server_received, server_received,
                                      (uint64_t)received);
}

static void constant_offset(void) {
    const int64_t offset = -11714794000LL;
    eaf_sendspin_time_filter_t f;
    eaf_sendspin_time_filter_init(&f);
    CHECK(!eaf_sendspin_time_synchronized(&f));
    CHECK(eaf_sendspin_time_error(&f) == INT64_MAX);

    int64_t now = 1000000;
    for (unsigned i = 0; i < 20; ++i) {
        feed(&f, now, 500, offset, 0.0);
        now += 200000;
    }
    CHECK(eaf_sendspin_time_synchronized(&f));
    CHECK(f.count == 20);
    CHECK(fabs(f.offset - (double)offset) < 5.0);
    CHECK(fabs(f.drift) < 1e-9);
    CHECK(eaf_sendspin_time_error(&f) < 1000);

    for (int64_t t = now; t < now + 5000000; t += 250000) {
        int64_t mapped = eaf_sendspin_compute_server_time(&f, t);
        CHECK(llabs(mapped - server_time(t, offset, 0.0)) < 50);
        CHECK(eaf_sendspin_compute_client_time(&f, mapped) == t);
    }

    /* Non-monotonic time_added is ignored. */
    uint32_t count = f.count;
    double offset_before = f.offset;
    eaf_sendspin_time_filter_update(&f, offset + 1000000, 1, 1u);
    CHECK(f.count == count && f.offset == offset_before);
}

static void drift_estimate(void) {
    const int64_t offset = 250000000LL;
    const double drift = -3e-6;
    eaf_sendspin_time_filter_t f;
    eaf_sendspin_time_filter_init(&f);

    int64_t now = 500000;
    for (unsigned i = 0; i < 60; ++i) {
        feed(&f, now, 200, offset, drift);
        now += 1000000;
    }
    CHECK(eaf_sendspin_time_synchronized(&f));
    CHECK(fabs(f.drift - drift) < 0.3e-6);
    for (int64_t t = now; t < now + 10000000; t += 500000) {
        int64_t mapped = eaf_sendspin_compute_server_time(&f, t);
        CHECK(llabs(mapped - server_time(t, offset, drift)) < 200);
        CHECK(eaf_sendspin_compute_client_time(&f, mapped) == t);
    }
}

static void reset_and_edges(void) {
    eaf_sendspin_time_filter_t f;
    eaf_sendspin_time_filter_init(&f);
    feed(&f, 1000000, 100, 4242, 0.0);
    CHECK(f.count == 1 && !eaf_sendspin_time_synchronized(&f) && f.element.offset == 4242.0);
    feed(&f, 1200000, 100, 4242, 0.0);
    CHECK(f.count == 2 && eaf_sendspin_time_synchronized(&f));
    eaf_sendspin_time_filter_reset(&f);
    CHECK(f.count == 0 && !eaf_sendspin_time_synchronized(&f) &&
          eaf_sendspin_time_error(&f) == INT64_MAX);
    CHECK(eaf_sendspin_compute_server_time(&f, 123) == 123);
    CHECK(eaf_sendspin_compute_client_time(&f, 456) == 456);
}

int main(void) {
    constant_offset();
    drift_estimate();
    reset_and_edges();
    puts("sendspin sync PASS");
    return 0;
}
