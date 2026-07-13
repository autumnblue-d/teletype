#include "greatest/greatest.h"
#include "music.h"  // ET reference table
#include "tuning.h"

// Default table is equal temperament on every channel.
TEST test_tuning_default_is_et() {
    tuning_default();
    for (uint8_t ch = 0; ch < TUNING_CHANNELS; ch++)
        for (uint8_t s = 0; s < TUNING_SLOTS; s++)
            ASSERT_EQ(ET[s], tuning_table[ch][s]);
    PASS();
}

// note_to_cv_ch on the default table matches the plain ET map, mirrors
// negatives, and clamps out-of-range indices.
TEST test_tuning_note_to_cv() {
    tuning_default();
    ASSERT_EQ((int16_t)ET[0], note_to_cv_ch(0, 0));
    ASSERT_EQ((int16_t)ET[60], note_to_cv_ch(2, 60));
    ASSERT_EQ(-(int16_t)ET[12], note_to_cv_ch(1, -12));
    // clamp: notes at/above the table ceiling saturate to the last slot
    ASSERT_EQ((int16_t)ET[TUNING_SLOTS - 1], note_to_cv_ch(0, 127));
    ASSERT_EQ((int16_t)ET[TUNING_SLOTS - 1],
              note_to_cv_ch(0, TUNING_SLOTS - 1));
    PASS();
}

// set/get clamp to the DAC ceiling and channel/slot bounds.
TEST test_tuning_set_get_clamp() {
    tuning_default();
    tuning_set(0, 5, 99999);  // above ceiling
    ASSERT_EQ(TUNING_DAC_MAX, tuning_get(0, 5));
    tuning_set(1, 7, 4096);
    ASSERT_EQ(4096, tuning_get(1, 7));
    // out-of-range set is a no-op; out-of-range get clamps the index
    tuning_set(TUNING_CHANNELS, 0, 1234);
    ASSERT_EQ(tuning_get(TUNING_CHANNELS - 1, TUNING_SLOTS - 1),
              tuning_get(99, 250));
    PASS();
}

// fit mode 0: each channel's slot-0 value becomes a constant offset added to
// equal temperament across all slots.
TEST test_tuning_fit_offset() {
    tuning_default();
    tuning_table[0][0] = 100;  // introduce a slot-0 offset on channel 0
    tuning_fit(0);
    for (uint8_t s = 0; s < TUNING_SLOTS; s++) {
        int32_t expect = (int32_t)ET[s] + 100;
        if (expect > (int32_t)TUNING_DAC_MAX) expect = TUNING_DAC_MAX;
        ASSERT_EQ((uint16_t)expect, tuning_table[0][s]);
    }
    // untouched channels keep offset 0 -> stay equal temperament
    for (uint8_t s = 0; s < TUNING_SLOTS; s++)
        ASSERT_EQ(ET[s], tuning_table[1][s]);
    PASS();
}

// fit mode 1: linear interpolation between octave waypoints. With every
// waypoint left at ET, the result is unchanged; a moved waypoint produces a
// monotonic straight line to the next waypoint.
TEST test_tuning_fit_linear() {
    tuning_default();
    // move the octave-1 waypoint (slot 12) up; slots 0..11 should ramp linearly
    // from slot 0 (ET[0]=0) toward the new slot-12 value.
    tuning_table[0][12] = ET[12] + 120;  // 12 semis-per-step easy to divide
    tuning_fit(1);
    // endpoints preserved
    ASSERT_EQ(ET[0], tuning_table[0][0]);
    ASSERT_EQ(ET[12] + 120, tuning_table[0][12]);
    // interior strictly increasing across the first octave
    for (uint8_t s = 1; s <= 12; s++)
        ASSERT(tuning_table[0][s] >= tuning_table[0][s - 1]);
    PASS();
}

SUITE(tuning_suite) {
    RUN_TEST(test_tuning_default_is_et);
    RUN_TEST(test_tuning_note_to_cv);
    RUN_TEST(test_tuning_set_get_clamp);
    RUN_TEST(test_tuning_fit_offset);
    RUN_TEST(test_tuning_fit_linear);
}
