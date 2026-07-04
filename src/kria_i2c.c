// Kria i2c follower output -- see kria_i2c.h.

#include "kria_i2c.h"

#include "ii.h"             // TELEXO, JF_ADDR, JF_VOX addresses/commands
#include "kria_binding.h"   // kria_note_to_cv
#include "kria_engine.h"    // KR_I2C_* bits
#include "ops/telex.h"      // TXSend, TO_CV_N, TO_TR
#include "teletype_io.h"    // tele_ii_tx

// Just Friends VOX velocity used as the "gate on" level (unity, 14-bit).
#define KR_JF_VELOCITY 16384

void kria_i2c_note(uint8_t followers, uint8_t track, int16_t semitones,
                   uint8_t on) {
    uint8_t out = track + 1;  // followers are 1-indexed

    // TELEXo: set the output's pitch by note number (TXo does V/oct), gate on TR.
    if (followers & KR_I2C_TXO) {
        if (on) {
            TXSend(TELEXO, TO_CV_N, out, semitones, true);
            TXSend(TELEXO, TO_TR, out, 1, true);
        }
        else {
            TXSend(TELEXO, TO_TR, out, 0, true);
        }
    }

    // Just Friends: JF.VOX(voice, pitch, velocity). Pitch is the same 14-bit ET
    // value the jacks use; velocity 0 releases the voice.
    if (followers & KR_I2C_JF) {
        int16_t pitch = kria_note_to_cv(semitones);
        int16_t vel = on ? KR_JF_VELOCITY : 0;
        uint8_t d[] = { JF_VOX,          (uint8_t)out,   pitch >> 8,
                        pitch & 0xff,    vel >> 8,       vel & 0xff };
        tele_ii_tx(JF_ADDR, d, 6);
    }
}
