#ifndef _MEADOWPHYSICS_CLOCK_H_
#define _MEADOWPHYSICS_CLOCK_H_

// Meadowphysics clock: the rough/fine tempo encoding. The tempo model +
// internal/external phase arbitration is shared with Kria -- see grid_clock.h;
// the MP mode shell holds a grid_clock_t and passes the MP_CLOCK_PERIOD_*
// bounds below to grid_clock_init().

#include <stdint.h>

#include "grid_clock.h"  // grid_clock_t + shared phase/period arbitration

// Edge-interval bounds (ms), matching Ansible's clock_period range.
#define MP_CLOCK_PERIOD_MIN 20
#define MP_CLOCK_PERIOD_MAX 265
#define MP_CLOCK_PERIOD_DEFAULT 96

// Ansible's rough/fine tempo encoding: period = 20 + rough*16 + fine, clamped
// to [MIN, MAX]. rough and fine are 0..15 (a grid row each).
uint16_t mp_clock_period_from_rough_fine(uint8_t rough, uint8_t fine);

#endif
