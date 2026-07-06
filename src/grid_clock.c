// Shared clock core for the grid apps -- see grid_clock.h.

#include "grid_clock.h"

uint16_t grid_clock_clamp(uint16_t val, uint16_t min, uint16_t max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

void grid_clock_init(grid_clock_t* c, uint16_t period_min, uint16_t period_max,
                     uint16_t period_default) {
    c->phase = 0;
    c->external = false;
    c->metro = false;
    c->period_min = period_min;
    c->period_max = period_max;
    c->period = grid_clock_clamp(period_default, period_min, period_max);
}

void grid_clock_set_period(grid_clock_t* c, uint16_t period_ms) {
    c->period = grid_clock_clamp(period_ms, c->period_min, c->period_max);
}

void grid_clock_set_external(grid_clock_t* c, bool on) {
    c->external = on;
}

uint8_t grid_clock_internal_fire(grid_clock_t* c, uint8_t* phase_out) {
    if (c->external || c->metro) return 0;
    c->phase ^= 1;
    *phase_out = c->phase;
    return 1;
}

uint8_t grid_clock_external_edge(grid_clock_t* c, uint8_t level,
                                 uint8_t* phase_out) {
    if (!c->external) return 0;
    c->phase = level ? 1 : 0;
    *phase_out = c->phase;
    return 1;
}
