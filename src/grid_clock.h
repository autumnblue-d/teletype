#ifndef _GRID_CLOCK_H_
#define _GRID_CLOCK_H_

// Shared clock core for the ported grid apps (Kria, Meadowphysics).
//
// Both apps drive their sequencer, Ansible-style, from a soft timer that fires
// every `period` ms and TOGGLES a phase 0<->1 on each fire -- so the period is
// the *edge interval* and a full on/off cycle takes two fires. The on-edge
// (phase 1) advances the sequencer / fires notes; the off-edge (phase 0) is
// app-specific (Kria clears gates via note-off timers, MP clears them on the
// off-edge). Either app can instead be clocked from a Tr input, in which case
// the internal timer is suppressed and each Tr edge sets the phase directly.
//
// This module is the hardware-abstract, host-testable half. Firmware wiring
// (the soft timer, the engine instance, the mode-active flag) lives in each
// mode shell; the per-app period bounds are passed to grid_clock_init(), and
// the app-specific timing tails (Kria's duration/repeat scaling, MP's
// rough/fine tempo encoding) stay in kria_clock.h / meadowphysics_clock.h.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t phase;        // current output phase, 0 or 1
    bool external;        // true = advance from Tr input, ignore internal timer
    uint16_t period;      // internal edge interval in ms
    uint16_t period_min;  // clamp bounds for set_period (from init)
    uint16_t period_max;
} grid_clock_t;

// Clamp `val` to [min, max].
uint16_t grid_clock_clamp(uint16_t val, uint16_t min, uint16_t max);

// Initialise: internal mode, phase 0, period = clamp(period_default), and
// remember [period_min, period_max] for later set_period() calls.
void grid_clock_init(grid_clock_t* c, uint16_t period_min, uint16_t period_max,
                     uint16_t period_default);

// Set the internal edge interval (ms), clamped to the [min, max] from init.
void grid_clock_set_period(grid_clock_t* c, uint16_t period_ms);

// Enable/disable external clocking. On enable the internal timer is ignored
// and the sequencer only advances on Tr edges; on disable the timer resumes.
void grid_clock_set_external(grid_clock_t* c, bool on);

// Internal timer fired. In internal mode: toggle phase, write it to *phase_out,
// return 1 (caller should run the engine clock with *phase_out). In external
// mode: suppress and return 0.
uint8_t grid_clock_internal_fire(grid_clock_t* c, uint8_t* phase_out);

// A Tr-input edge arrived (external clock), `level` = the input pin state.
// TT's handler_Trigger fires on both edges, so the external clock is
// gate-driven: level high -> phase 1, level low -> phase 0; one pulse is one
// step. In external mode: set phase from level, write it to *phase_out,
// return 1. In internal mode: ignore, return 0.
uint8_t grid_clock_external_edge(grid_clock_t* c, uint8_t level,
                                 uint8_t* phase_out);

#endif
