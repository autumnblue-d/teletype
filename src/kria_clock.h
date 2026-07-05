#ifndef _KRIA_CLOCK_H_
#define _KRIA_CLOCK_H_

// Kria clock: the real-tick scaling that the engine deliberately defers (see
// kria_engine.h). The tempo model + internal/external phase arbitration is
// shared with Meadowphysics -- see grid_clock.h; the Kria mode shell holds a
// grid_clock_t and passes the KR_CLOCK_PERIOD_* bounds below to
// grid_clock_init().

#include <stdint.h>

#include "grid_clock.h"  // grid_clock_t + shared phase/period arbitration

// Internal edge interval (ms) bounds. Ansible's default_kria clock_period = 60.
#define KR_CLOCK_PERIOD_MIN 10
#define KR_CLOCK_PERIOD_MAX 1000
#define KR_CLOCK_PERIOD_DEFAULT 60

// ---- real-tick scaling (deferred from the engine) ----
//
// The engine records rt.dur_unscaled = (dur[step]+1) * (dur_mul<<2) and the
// latched rpt. The shell measures the per-track clock length in ticks
// (clock_delta) and converts to timer periods here, matching Ansible:
//   gate length ticks = dur_unscaled * (clock_delta * tmul_tr) / 384
//   repeat spacing    = (clock_delta * tmul_tr) / rpt
// where tmul_tr is the track's trigger-param divider.

// Gate length in ticks for one trigger. Returns 0 if inputs collapse to 0.
uint16_t kria_clock_scale_duration(uint16_t dur_unscaled, uint32_t clock_delta,
                                   uint8_t tmul_tr);

// Spacing between repeats in ticks. rpt is clamped to >= 1.
uint32_t kria_clock_repeat_ticks(uint32_t clock_delta, uint8_t tmul_tr,
                                 uint8_t rpt);

#endif
