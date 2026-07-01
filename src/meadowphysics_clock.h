#ifndef _MEADOWPHYSICS_CLOCK_H_
#define _MEADOWPHYSICS_CLOCK_H_

// Meadowphysics clock: tempo model + internal/external phase arbitration.
//
// Ansible drives clock_mp() from a soft timer that fires every clock_period ms
// and TOGGLES a phase 0<->1 on each fire (ansible/src/main.c:138) -- so the
// period is the *edge interval* and a full on/off cycle takes two fires. The
// on-edge (phase 1) steps counters and fires notes; the off-edge (phase 0)
// clears gates. This module reproduces that timing and the internal/external
// source arbitration in a hardware-abstract, host-testable form.
//
// Firmware wiring is deliberately NOT here (it needs the engine instance and
// the mode-active flag): the mode shell registers a softTimer whose callback
// calls mp_clock_internal_fire() and, when that returns 1, mp_engine_clock();
// and handler_Trigger() calls mp_clock_external_edge() for Tr input 1 (A3).

#include <stdbool.h>
#include <stdint.h>

// Edge-interval bounds (ms), matching Ansible's clock_period range.
#define MP_CLOCK_PERIOD_MIN 20
#define MP_CLOCK_PERIOD_MAX 265
#define MP_CLOCK_PERIOD_DEFAULT 96

typedef struct {
    uint8_t phase;    // current output phase, 0 or 1
    bool external;    // true = advance from Tr input, ignore internal timer
    uint16_t period;  // internal edge interval in ms
} mp_clock_t;

void mp_clock_init(mp_clock_t* c);

// Set the internal edge interval (ms), clamped to [MIN, MAX].
void mp_clock_set_period(mp_clock_t* c, uint16_t period_ms);

// Ansible's rough/fine tempo encoding: period = 20 + rough*16 + fine, clamped
// to [MIN, MAX]. rough and fine are 0..15 (a grid row each).
uint16_t mp_clock_period_from_rough_fine(uint8_t rough, uint8_t fine);

// Enable/disable external clocking. On enable, the internal timer is ignored
// and the sequencer only advances on Tr edges; on disable the internal timer
// resumes ownership.
void mp_clock_set_external(mp_clock_t* c, bool on);

// Internal timer fired. In internal mode: toggle phase, write it to *phase_out,
// return 1 (caller should run mp_engine_clock(*phase_out)). In external mode:
// suppress and return 0.
uint8_t mp_clock_internal_fire(mp_clock_t* c, uint8_t* phase_out);

// A Tr-input edge arrived (external clock), `level` = the input pin state.
// TT's handler_Trigger fires on both edges, so the external clock is
// gate-driven: level high -> phase 1 (step + notes), level low -> phase 0
// (clear gates); one pulse is one step. In external mode: set phase from
// level, write it to *phase_out, return 1. In internal mode: ignore, return 0.
uint8_t mp_clock_external_edge(mp_clock_t* c, uint8_t level,
                               uint8_t* phase_out);

#endif
