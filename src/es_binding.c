// Earthsea output binding -- see es_binding.h.

#include "es_binding.h"

#include <stddef.h>  // NULL

#include "helpers.h"      // note_to_cv (plain ET, for the i2c fan-out)
#include "kria_i2c.h"     // shared i2c follower fan-out
#include "teletype_io.h"  // tele_tr, tele_cv
#include "tuning.h"       // note_to_cv_ch (per-channel tuning table)

void es_binding_note_on(uint8_t voice, int16_t semitones, uint16_t duration) {
    if (voice >= ES_NUM_VOICES) return;
    // i2c followers stay on the plain ET map (matches Ansible); the module CV
    // out is retuned per channel via the tuning table.
    kria_i2c_set_voice(voice, semitones, duration);
    kria_i2c_cv(voice, note_to_cv(semitones));
    tele_cv(voice, note_to_cv_ch(voice, semitones), 0);
    tele_tr(voice, 1);
    kria_i2c_tr(voice, 1);
}

void es_binding_note_off(uint8_t voice) {
    if (voice >= ES_NUM_VOICES) return;
    tele_tr(voice, 0);
    kria_i2c_tr(voice, 0);
}

static void bind_note_on(void* ctx, uint8_t voice, int16_t semitones,
                         uint16_t duration) {
    (void)ctx;
    es_binding_note_on(voice, semitones, duration);
}

static void bind_note_off(void* ctx, uint8_t voice) {
    (void)ctx;
    es_binding_note_off(voice);
}

static const es_output_t OUTPUT = { .note_on = bind_note_on,
                                    .note_off = bind_note_off,
                                    .ctx = NULL };

const es_output_t* es_binding_output(void) {
    return &OUTPUT;
}
