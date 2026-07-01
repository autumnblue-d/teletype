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

// Convert an engine scale degree / semitone (as passed to out.cv by
// mp_note_on) into a raw 14-bit CV value using Teletype's equal-temperament
// table (ET) -- identical to the N op's note_number_to_volts(). The per-channel
// calibration offset is applied later inside tele_cv(). Note is clamped to
// +/-127; negatives map below 0V.
int16_t mp_note_to_cv(int16_t note);

// The output vtable routing engine events to TT hardware (tele_tr / tele_cv).
// Pass to mp_engine_init(); ctx is unused. Gates are held levels (set on
// note-on, cleared on note-off), not fixed pulses -- the engine owns timing.
const mp_output_t* mp_binding_output(void);

#endif
