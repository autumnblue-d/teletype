// Kria clock -- see kria_clock.h.

#include "kria_clock.h"

static uint16_t clamp_period(uint16_t p) {
    if (p < KR_CLOCK_PERIOD_MIN) return KR_CLOCK_PERIOD_MIN;
    if (p > KR_CLOCK_PERIOD_MAX) return KR_CLOCK_PERIOD_MAX;
    return p;
}

void kria_clock_init(kria_clock_t* c) {
    c->phase = 0;
    c->external = false;
    c->period = KR_CLOCK_PERIOD_DEFAULT;
}

void kria_clock_set_period(kria_clock_t* c, uint16_t period_ms) {
    c->period = clamp_period(period_ms);
}

void kria_clock_set_external(kria_clock_t* c, bool on) {
    c->external = on;
}

uint8_t kria_clock_internal_fire(kria_clock_t* c, uint8_t* phase_out) {
    if (c->external) return 0;
    c->phase ^= 1;
    *phase_out = c->phase;
    return 1;
}

uint8_t kria_clock_external_edge(kria_clock_t* c, uint8_t level,
                                 uint8_t* phase_out) {
    if (!c->external) return 0;
    c->phase = level ? 1 : 0;
    *phase_out = c->phase;
    return 1;
}

uint16_t kria_clock_scale_duration(uint16_t dur_unscaled, uint32_t clock_delta,
                                   uint8_t tmul_tr) {
    // Ansible: dur = (u16)(unscaled * clock_scale),
    //          clock_scale = (clock_deltas * tmul[mTr]) / 384.0
    float clock_scale = ((float)clock_delta * (float)tmul_tr) / 384.0f;
    return (uint16_t)((float)dur_unscaled * clock_scale);
}

uint32_t kria_clock_repeat_ticks(uint32_t clock_delta, uint8_t tmul_tr,
                                 uint8_t rpt) {
    if (rpt == 0) rpt = 1;
    return (clock_delta * (uint32_t)tmul_tr) / (uint32_t)rpt;
}
