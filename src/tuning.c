#include "tuning.h"

#include "music.h"  // ET (equal-temperament semitone -> 14-bit DAC)

uint16_t tuning_table[TUNING_CHANNELS][TUNING_SLOTS];

static uint16_t clamp_dac(int32_t v) {
    if (v < 0) return 0;
    if (v > (int32_t)TUNING_DAC_MAX) return TUNING_DAC_MAX;
    return (uint16_t)v;
}

void tuning_default(void) {
    for (uint8_t ch = 0; ch < TUNING_CHANNELS; ch++)
        for (uint8_t s = 0; s < TUNING_SLOTS; s++) tuning_table[ch][s] = ET[s];
}

uint16_t tuning_get(uint8_t ch, uint8_t slot) {
    if (ch >= TUNING_CHANNELS) ch = TUNING_CHANNELS - 1;
    if (slot >= TUNING_SLOTS) slot = TUNING_SLOTS - 1;
    return tuning_table[ch][slot];
}

void tuning_set(uint8_t ch, uint8_t slot, uint16_t dac) {
    if (ch >= TUNING_CHANNELS || slot >= TUNING_SLOTS) return;
    tuning_table[ch][slot] = clamp_dac((int32_t)dac);
}

void tuning_fit(uint8_t mode) {
    if (mode == 0) {  // fixed offset per channel
        for (uint8_t ch = 0; ch < TUNING_CHANNELS; ch++) {
            int32_t offset = tuning_table[ch][0];
            for (uint8_t s = 0; s < TUNING_SLOTS; s++)
                tuning_table[ch][s] = clamp_dac((int32_t)ET[s] + offset);
        }
    }
    else if (mode == 1) {  // linear interpolation between octave waypoints
        for (uint8_t ch = 0; ch < TUNING_CHANNELS; ch++) {
            // Q16.16 fixed point, matching Ansible's libfixmath fit. Keep the
            // last octave's slope for the final (10th) octave (no waypoint
            // above it), so the top octave is extrapolated, not flat.
            int64_t step = 0;
            for (uint8_t oct = 0; oct < TUNING_OCTAVES; oct++) {
                int64_t acc = (int64_t)tuning_table[ch][oct * 12] << 16;
                if (oct < TUNING_OCTAVES - 1) {
                    int32_t delta = (int32_t)tuning_table[ch][(oct + 1) * 12] -
                                    (int32_t)tuning_table[ch][oct * 12];
                    step = ((int64_t)delta << 16) / 12;
                }
                for (uint8_t k = oct * 12; k < (oct + 1) * 12; k++) {
                    int64_t rounded = (acc >= 0) ? acc + 0x8000 : acc - 0x8000;
                    tuning_table[ch][k] = clamp_dac((int32_t)(rounded >> 16));
                    acc += step;
                }
            }
        }
    }
}

int16_t note_to_cv_ch(uint8_t ch, int16_t note) {
    if (note < 0) {
        if (note < -(TUNING_SLOTS - 1)) note = -(TUNING_SLOTS - 1);
        return -(int16_t)tuning_get(ch, (uint8_t)(-note));
    }
    if (note > TUNING_SLOTS - 1) note = TUNING_SLOTS - 1;
    return (int16_t)tuning_get(ch, (uint8_t)note);
}
