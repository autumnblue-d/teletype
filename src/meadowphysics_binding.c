// Meadowphysics output binding -- see meadowphysics_binding.h.

#include "meadowphysics_binding.h"

#include <stddef.h>  // NULL

#include "music.h"        // ET, ET_SIZE (equal-temperament semitone -> 14-bit)
#include "teletype_io.h"  // tele_tr, tele_cv

int16_t mp_note_to_cv(int16_t note) {
    // Mirrors note_number_to_volts() in src/ops/maths.c so MP pitch tracks the
    // same tuning/calibration as the N op.
    if (note < 0) {
        if (note < -127) note = -127;
        return -(int16_t)ET[-note];
    }
    if (note > 127) note = 127;
    return (int16_t)ET[note];
}

static void bind_tr(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    tele_tr(ch, on);
}

static void bind_cv(void* ctx, uint8_t ch, int16_t note) {
    (void)ctx;
    tele_cv(ch, mp_note_to_cv(note), 0);
}

static void bind_cv_gate(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    tele_cv(ch, on ? MP_CV_FULL : 0, 0);
}

static const mp_output_t OUTPUT = {
    .tr = bind_tr, .cv = bind_cv, .cv_gate = bind_cv_gate, .ctx = NULL
};

const mp_output_t* mp_binding_output(void) {
    return &OUTPUT;
}
