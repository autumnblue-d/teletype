#ifndef _TUNING_H_
#define _TUNING_H_

#include <stdint.h>

// Per-output CV tuning table (ported from Ansible's advanced "tuning" feature,
// https://monome.org/docs/ansible/advanced/#tuning). Replaces the fixed
// equal-temperament map with an editable per-channel lookup so users can
// correct DAC/VCO mismatch or reprogram for just intonation / microtuning.
//
// Only the ported grid apps (Kria / Meadowphysics / Earthsea) route their note
// output through this table -- Teletype's native ops (N, QT, CV ...) keep the
// plain ET map. The i2c CV fan-out also stays on plain ET, matching Ansible.
//
// The table is a global bank stored in NVRAM (see flash_get/update_tuning),
// NOT per-scene. This module owns only the RAM working copy + pure helpers;
// flash load/save lives in module/flash.c.

#define TUNING_CHANNELS 4
#define TUNING_SLOTS 120       // 10 octaves x 12 semitones (Ansible layout)
#define TUNING_OCTAVES 10      // fit waypoints, one per octave
#define TUNING_DAC_MAX 16383u  // DAC_10V ceiling (tele_cv clamps to this)

// RAM working copy: [channel][semitone] -> 14-bit DAC value. Edited live by the
// grid tuning view; persisted to flash only on an explicit save.
extern uint16_t tuning_table[TUNING_CHANNELS][TUNING_SLOTS];

// Reset the RAM copy to equal temperament (ET). Does not touch flash.
void tuning_default(void);

// Slot accessors with bounds clamping.
uint16_t tuning_get(uint8_t ch, uint8_t slot);
void tuning_set(uint8_t ch, uint8_t slot, uint16_t dac);

// Re-fit the whole RAM table from the current octave waypoints:
//   mode 0 -- fixed offset per channel: each channel keeps its slot-0 value as
//             a constant offset added to equal temperament.
//   mode 1 -- linear interpolation between octave waypoints (slots
//   0,12,...,108),
//             extrapolating the final octave with the previous slope.
void tuning_fit(uint8_t mode);

// Convert a note/semitone index to a 14-bit CV value via channel `ch`'s tuning
// table. Companion to note_to_cv() in helpers.c (which stays on plain ET for
// the native ops). Clamps the magnitude into the table range; negative notes
// mirror below 0V just like note_to_cv().
int16_t note_to_cv_ch(uint8_t ch, int16_t note);

#endif
