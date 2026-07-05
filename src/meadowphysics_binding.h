#ifndef _MEADOWPHYSICS_BINDING_H_
#define _MEADOWPHYSICS_BINDING_H_

// Output binding: maps the hardware-abstract Meadowphysics engine events
// (mp_output_t: tr / cv / cv_gate) onto Teletype's CV/TR outputs. The engine
// already does voice-mode routing (which channel each row hits); this layer
// only converts a scale degree to a calibrated CV value and drives the jacks.

#include <stdint.h>

#include "meadowphysics_engine.h"

// Full-scale 14-bit DAC value: drives a CV jack as a 0/10V gate in 8T mode
// (Ansible's DAC_10V). tele_cv() clamps its input to this ceiling.
#define MP_CV_FULL 16383

// Scale degree / semitone -> CV conversion is shared: see note_to_cv() in
// helpers.h (used by the N op and all three grid-app bindings).

// The output vtable routing engine events to TT hardware (tele_tr / tele_cv).
// Pass to mp_engine_init(); ctx is unused. Gates are held levels (set on
// note-on, cleared on note-off), not fixed pulses -- the engine owns timing.
const mp_output_t* mp_binding_output(void);

#endif
