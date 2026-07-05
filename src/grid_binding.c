// Shared output-binding leaves for the grid apps -- see grid_binding.h.

#include "grid_binding.h"

#include "helpers.h"      // note_to_cv (shared ET semitone mapping)
#include "teletype_io.h"  // tele_tr, tele_cv

void grid_bind_tr(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    tele_tr(ch, on);
}

void grid_bind_cv(void* ctx, uint8_t ch, int16_t note) {
    (void)ctx;
    tele_cv(ch, note_to_cv(note), 0);
}
