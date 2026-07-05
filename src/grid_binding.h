#ifndef _GRID_BINDING_H_
#define _GRID_BINDING_H_

// Shared output-binding leaves for the grid apps. The Kria and Meadowphysics
// engine output vtables (kria_output_t / mp_output_t) share identical `tr` and
// `cv` function-pointer types, so both bind them to these two functions rather
// than each rolling its own identical copy. The per-app verbs that differ
// (Kria's cv_slew, MP's cv_gate) stay in their own binding files. `ctx` is
// unused (the engine already routes track n -> jack n).

#include <stdint.h>

// Gate: forward on/off to TR channel `ch`.
void grid_bind_tr(void* ctx, uint8_t ch, uint8_t on);

// Pitch: convert a semitone index to CV (note_to_cv) and write CV channel `ch`.
void grid_bind_cv(void* ctx, uint8_t ch, int16_t note);

#endif
