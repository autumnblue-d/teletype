// Meadowphysics clock -- see meadowphysics_clock.h.

#include "meadowphysics_clock.h"

static uint16_t clamp_period(uint16_t p) {
    if (p < MP_CLOCK_PERIOD_MIN) return MP_CLOCK_PERIOD_MIN;
    if (p > MP_CLOCK_PERIOD_MAX) return MP_CLOCK_PERIOD_MAX;
    return p;
}

void mp_clock_init(mp_clock_t* c) {
    c->phase = 0;
    c->external = false;
    c->period = MP_CLOCK_PERIOD_DEFAULT;
}

void mp_clock_set_period(mp_clock_t* c, uint16_t period_ms) {
    c->period = clamp_period(period_ms);
}

uint16_t mp_clock_period_from_rough_fine(uint8_t rough, uint8_t fine) {
    return clamp_period((uint16_t)(20 + rough * 16 + fine));
}

void mp_clock_set_external(mp_clock_t* c, bool on) {
    c->external = on;
}

uint8_t mp_clock_internal_fire(mp_clock_t* c, uint8_t* phase_out) {
    if (c->external) return 0;
    c->phase ^= 1;
    *phase_out = c->phase;
    return 1;
}

uint8_t mp_clock_external_edge(mp_clock_t* c, uint8_t level,
                               uint8_t* phase_out) {
    if (!c->external) return 0;
    c->phase = level ? 1 : 0;
    *phase_out = c->phase;
    return 1;
}
