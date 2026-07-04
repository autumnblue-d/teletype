#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // ssize_t

#include "dejavu.h"
#include "greatest/greatest.h"
#include "log.h"

// deja vu is stateful + float + RNG-driven, so these assert on STRUCTURE
// (periodicity, write-head motion, range, clamping) which hold for any RNG
// stream, rather than exact float values.

static random_state_t rng;

static dejavu_t make(float deja_vu, int length) {
    random_seed(&rng, 0x12345);
    dejavu_t d;
    dejavu_init(&d, &rng);
    dejavu_set_length(&d, length);
    dejavu_set_deja_vu(&d, deja_vu);
    return d;
}

// deja_vu = 0.5 -> p = 0 -> never mutate -> locked loop with period = length.
TEST test_locked_loop_period() {
    dejavu_t d = make(0.5f, 4);
    float v[8];
    for (int i = 0; i < 8; i++) { v[i] = dejavu_next(&d); }
    for (int k = 0; k < 4; k++) { ASSERT_EQ(v[k], v[k + 4]); }
    // a locked loop never advances the write head
    ASSERT_EQ(0, d.loop_write_head);
    PASS();
}

// deja_vu = 0.0 -> p = 1 -> always regenerate -> write head advances each step.
TEST test_random_advances_write_head() {
    dejavu_t d = make(0.0f, 8);
    dejavu_next(&d);
    dejavu_next(&d);
    dejavu_next(&d);
    ASSERT_EQ(3, d.loop_write_head);
    PASS();
}

TEST test_length_clamped() {
    dejavu_t d = make(0.5f, 8);
    dejavu_set_length(&d, 0);
    ASSERT_EQ(8, dejavu_get_length(&d));  // rejected
    dejavu_set_length(&d, 17);
    ASSERT_EQ(8, dejavu_get_length(&d));  // rejected
    dejavu_set_length(&d, 1);
    ASSERT_EQ(1, dejavu_get_length(&d));
    dejavu_set_length(&d, 16);
    ASSERT_EQ(16, dejavu_get_length(&d));
    PASS();
}

TEST test_deja_vu_clamped() {
    dejavu_t d = make(0.5f, 8);
    dejavu_set_deja_vu(&d, -1.0f);
    ASSERT(dejavu_get_deja_vu(&d) == 0.0f);
    dejavu_set_deja_vu(&d, 2.0f);
    ASSERT(dejavu_get_deja_vu(&d) == 1.0f);
    PASS();
}

TEST test_output_range() {
    // across all three regimes, output stays in [0,1)
    float djs[3] = { 0.0f, 0.5f, 1.0f };
    for (int j = 0; j < 3; j++) {
        dejavu_t d = make(djs[j], 8);
        for (int i = 0; i < 64; i++) {
            float v = dejavu_next(&d);
            ASSERT(v >= 0.0f);
            ASSERT(v < 1.0f);
        }
    }
    PASS();
}

TEST test_record_resets() {
    dejavu_t d = make(0.0f, 8);
    dejavu_next(&d);
    dejavu_next(&d);  // write head now 2, step advanced
    dejavu_record(&d);
    ASSERT_EQ(0, d.loop_write_head);
    ASSERT_EQ(0, d.step);
    PASS();
}

SUITE(dejavu_suite) {
    log_init();
    RUN_TEST(test_locked_loop_period);
    RUN_TEST(test_random_advances_write_head);
    RUN_TEST(test_length_clamped);
    RUN_TEST(test_deja_vu_clamped);
    RUN_TEST(test_output_range);
    RUN_TEST(test_record_resets);
}
