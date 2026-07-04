#ifndef _KRIA_CLOCK_H_
#define _KRIA_CLOCK_H_

// Kria clock: tempo model + internal/external phase arbitration, plus the
// real-tick scaling that the engine deliberately defers (see kria_engine.h).
//
// Like Ansible, a soft timer fires every clock_period ms and TOGGLES a phase
// 0<->1; the on-edge (phase 1) advances the sequencer, the off-edge does
// nothing (Kria clears gates via note-off timers, not the clock). This module
// reproduces the timing + source arbitration in a hardware-abstract,
// host-testable form. Firmware wiring lives in the mode shell (it needs the
// engine instance + active flag): the shell's timer callback calls
// kria_clock_internal_fire() and, when it returns 1, kria_engine_clock();
// handler_Trigger() calls kria_clock_external_edge() for the Kria clock input.

#include <stdbool.h>
#include <stdint.h>

// Internal edge interval (ms) bounds. Ansible's default_kria clock_period = 60.
#define KR_CLOCK_PERIOD_MIN 10
#define KR_CLOCK_PERIOD_MAX 1000
#define KR_CLOCK_PERIOD_DEFAULT 60

typedef struct {
    uint8_t phase;    // current output phase, 0 or 1
    bool external;    // true = advance from Tr input, ignore internal timer
    uint16_t period;  // internal edge interval in ms
} kria_clock_t;

void kria_clock_init(kria_clock_t* c);

// Set the internal edge interval (ms), clamped to [MIN, MAX].
void kria_clock_set_period(kria_clock_t* c, uint16_t period_ms);

// Enable/disable external clocking.
void kria_clock_set_external(kria_clock_t* c, bool on);

// Internal timer fired. In internal mode: toggle phase, write to *phase_out,
// return 1 (caller runs kria_engine_clock(*phase_out)). External: return 0.
uint8_t kria_clock_internal_fire(kria_clock_t* c, uint8_t* phase_out);

// A Tr-input edge arrived (external clock), `level` = the input pin state.
// External mode: set phase from level, write to *phase_out, return 1.
// Internal mode: ignore, return 0.
uint8_t kria_clock_external_edge(kria_clock_t* c, uint8_t level,
                                 uint8_t* phase_out);

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
