#ifndef _ES_BINDING_H_
#define _ES_BINDING_H_

// Output binding: maps the hardware-abstract Earthsea engine events
// (es_output_t: note_on / note_off) onto Teletype's CV/TR jacks and the
// shared i2c follower layer (kria_i2c, the same global follower bank Kria
// and MP drive; ES voice n = follower track n).
//
// Voice n -> CV n (pitch, ET via note_to_cv() in helpers.h -- same tuning /
// calibration as the N op) + TR n (gate). Pitch is written before the gate
// rises, like Ansible's set_cv_note/dac_update_now/set_tr sequence.
//
// The fixed-edge `duration` on note_on is NOT handled here -- the mode shell
// wraps this vtable, schedules the note-off timer, and calls
// es_engine_note_off_voice() when it fires (see EARTHSEA_PORT_PLAN.md §7).

#include <stdint.h>

#include "es_engine.h"

// Plain primitives for the shell's wrapper vtable.
void es_binding_note_on(uint8_t voice, int16_t semitones, uint16_t duration);
void es_binding_note_off(uint8_t voice);

// Ready-made vtable (jacks + i2c, no duration timers) for tests and headless
// use. Pass to es_engine_init(); ctx is unused.
const es_output_t* es_binding_output(void);

#endif
