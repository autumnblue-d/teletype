#ifndef _KRIA_BINDING_H_
#define _KRIA_BINDING_H_

// Output binding: maps the hardware-abstract Kria engine events (kria_output_t:
// tr / cv / cv_slew) onto Teletype's CV/TR outputs. The engine already decides
// which track drives which jack (track n -> CV n + TR n) and computes the
// semitone index + glide amount; this layer only converts a semitone index to a
// calibrated CV value (ET) and forwards to the jacks.

#include <stdint.h>

#include "kria_engine.h"

// Convert an engine semitone index (as passed to out.cv) into a raw 14-bit CV
// value using Teletype's equal-temperament table (ET) -- identical to the N
// op's note_number_to_volts() and to Meadowphysics's mp_note_to_cv(), so Kria
// pitch tracks the same tuning/calibration. Per-channel calibration is applied
// later inside tele_cv(). Clamped to +/-127.
int16_t kria_note_to_cv(int16_t semitones);

// The output vtable routing engine events to TT hardware (tele_tr / tele_cv /
// tele_cv_slew). Pass to kria_engine_init(); ctx is unused. Gates are held
// levels (raised on trigger, cleared on note-off), the engine/shell own timing.
const kria_output_t* kria_binding_output(void);

#endif
