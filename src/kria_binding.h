#ifndef _KRIA_BINDING_H_
#define _KRIA_BINDING_H_

// Output binding: maps the hardware-abstract Kria engine events (kria_output_t:
// tr / cv / cv_slew) onto Teletype's CV/TR outputs. The engine already decides
// which track drives which jack (track n -> CV n + TR n) and computes the
// semitone index + glide amount; this layer only converts a semitone index to a
// calibrated CV value (ET) and forwards to the jacks.

#include <stdint.h>

#include "kria_engine.h"

// Semitone -> CV conversion is shared: see note_to_cv() in helpers.h (used by
// the N op and all three grid-app bindings for identical tuning/calibration).

// The output vtable routing engine events to TT hardware (tele_tr / tele_cv /
// tele_cv_slew). Pass to kria_engine_init(); ctx is unused. Gates are held
// levels (raised on trigger, cleared on note-off), the engine/shell own timing.
const kria_output_t* kria_binding_output(void);

#endif
