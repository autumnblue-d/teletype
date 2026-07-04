// Kria output binding -- see kria_binding.h.

#include "kria_binding.h"

#include <stddef.h>  // NULL

#include "music.h"        // ET (equal-temperament semitone -> 14-bit)
#include "teletype_io.h"  // tele_tr, tele_cv, tele_cv_slew

int16_t kria_note_to_cv(int16_t semitones) {
    // Mirrors note_number_to_volts() in src/ops/maths.c (and mp_note_to_cv) so
    // Kria pitch tracks the same tuning/calibration as the N op.
    if (semitones < 0) {
        if (semitones < -127) semitones = -127;
        return -(int16_t)ET[-semitones];
    }
    if (semitones > 127) semitones = 127;
    return (int16_t)ET[semitones];
}

static void bind_tr(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    tele_tr(ch, on);
}

static void bind_cv(void* ctx, uint8_t ch, int16_t semitones) {
    (void)ctx;
    tele_cv(ch, kria_note_to_cv(semitones), 0);
}

static void bind_cv_slew(void* ctx, uint8_t ch, uint16_t slew) {
    (void)ctx;
    tele_cv_slew(ch, (int16_t)slew);
}

static const kria_output_t OUTPUT = {
    .tr = bind_tr, .cv = bind_cv, .cv_slew = bind_cv_slew, .ctx = NULL
};

const kria_output_t* kria_binding_output(void) {
    return &OUTPUT;
}
