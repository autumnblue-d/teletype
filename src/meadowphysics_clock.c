// Meadowphysics clock -- see meadowphysics_clock.h. (Tempo/phase arbitration
// lives in grid_clock.c; only the rough/fine tempo encoding is MP-specific.)

#include "meadowphysics_clock.h"

uint16_t mp_clock_period_from_rough_fine(uint8_t rough, uint8_t fine) {
    return grid_clock_clamp((uint16_t)(20 + rough * 16 + fine),
                            MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX);
}
