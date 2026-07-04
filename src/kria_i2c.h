#ifndef _KRIA_I2C_H_
#define _KRIA_I2C_H_

// i2c follower output for native Kria: routes a track's pitch + gate to
// enabled follower modules (Ansible's i2c-leader feature). Additive to the CV/TR
// jacks. Reuses Teletype's existing follower protocols (telex.h TXSend / ii.h
// addresses) and pitch conversion (kria_note_to_cv). See KRIA_I2C_PLAN.md.
//
// Follower bits are KR_I2C_* (kria_engine.h). This phase supports TELEXo and
// Just Friends; track n -> follower output/voice n+1.

#include <stdint.h>

// Emit a note/gate for `track` (0-indexed) to every follower in the `followers`
// bitmask. On `on`: set pitch (from `semitones`) and raise the gate; otherwise
// lower the gate.
void kria_i2c_note(uint8_t followers, uint8_t track, int16_t semitones,
                   uint8_t on);

#endif
