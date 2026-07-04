#include "grids_helpers.h"

#include "grids_data.h"

static int clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Mutable Instruments U8Mix: linear interpolation, balance 0..255.
static uint8_t u8mix(uint8_t a, uint8_t b, uint8_t balance) {
    return (a * (255 - balance) + b * balance) >> 8;
}

// Bilinear read of the 5x5 topographic map. Verbatim ReadDrumMap, minus AVR8
// PROGMEM. Low 6 bits of x/y select the sub-cell interpolation balance
// (<< 2 scales 0..63 -> 0..252, truncated to uint8_t as in the original).
int grids_level(int instrument, int x, int y, int step) {
    instrument = clamp(instrument, 0, GRIDS_INSTRUMENTS - 1);
    x = clamp(x, 0, 255);
    y = clamp(y, 0, 255);
    step = ((step % GRIDS_STEPS) + GRIDS_STEPS) % GRIDS_STEPS;

    uint8_t i = (uint8_t)x >> 6;
    uint8_t j = (uint8_t)y >> 6;
    const uint8_t *a_map = grids_map[i][j];
    const uint8_t *b_map = grids_map[i + 1][j];
    const uint8_t *c_map = grids_map[i][j + 1];
    const uint8_t *d_map = grids_map[i + 1][j + 1];

    int offset = instrument * GRIDS_STEPS + step;
    uint8_t a = a_map[offset];
    uint8_t b = b_map[offset];
    uint8_t c = c_map[offset];
    uint8_t d = d_map[offset];

    uint8_t bx = (uint8_t)(x << 2);
    uint8_t by = (uint8_t)(y << 2);
    return u8mix(u8mix(a, b, bx), u8mix(c, d, bx), by);
}

int grids_trigger(int instrument, int x, int y, int density, int step) {
    int level = grids_level(instrument, x, y, step);
    // Grids: threshold = ~density; a step fires when level > threshold.
    uint8_t threshold = ~(uint8_t)clamp(density, 0, 255);
    return level > threshold ? 1 : 0;
}

int grids_accent(int instrument, int x, int y, int density, int step) {
    if (!grids_trigger(instrument, x, y, density, step)) return 0;
    return grids_level(instrument, x, y, step) > 192 ? 1 : 0;
}
