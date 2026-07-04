#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // ssize_t

#include "greatest/greatest.h"
#include "grids_helpers.h"
#include "log.h"

// Expected values are an oracle computed from the ORIGINAL Grids source
// (eurorack/grids/resources.cc nodes + pattern_generator.cc drum_map[5][5]),
// independent of src/grids_data.c. Matching them validates both the data
// transcription and the ReadDrumMap/U8Mix port.

TEST test_grids_level_known_points() {
    ASSERT_EQ(143, grids_level(0, 0, 0, 0));
    ASSERT_EQ(253, grids_level(0, 0, 0, 12));
    ASSERT_EQ(0, grids_level(1, 0, 0, 0));
    ASSERT_EQ(253, grids_level(2, 0, 0, 0));
    ASSERT_EQ(252, grids_level(0, 255, 255, 0));
    ASSERT_EQ(61, grids_level(0, 128, 128, 4));
    ASSERT_EQ(210, grids_level(2, 64, 192, 8));
    ASSERT_EQ(0, grids_level(0, 255, 0, 6));
    PASS();
}

TEST test_grids_level_clamp_and_wrap() {
    ASSERT_EQ(grids_level(0, 0, 0, 0), grids_level(0, 0, 0, 32));  // step wraps
    ASSERT_EQ(grids_level(0, 0, 0, 31), grids_level(0, 0, 0, -1));  // neg wraps
    // out-of-range instrument / x / y clamp to 2 / 255 / 255
    ASSERT_EQ(grids_level(2, 255, 255, 5), grids_level(9, 999, 999, 5));
    PASS();
}

TEST test_grids_trigger_density() {
    // density 255 => threshold 0 => fires wherever level > 0
    ASSERT_EQ(1, grids_trigger(0, 0, 0, 255, 0));  // level 143
    ASSERT_EQ(0, grids_trigger(0, 0, 0, 255, 1));  // level 0
    // density 0 => threshold 255 => never fires (level maxes at 254)
    ASSERT_EQ(0, grids_trigger(0, 0, 0, 0, 0));
    PASS();
}

TEST test_grids_accent() {
    // accent requires the step to fire AND level > 192
    ASSERT_EQ(0, grids_accent(0, 0, 0, 255, 0));   // fires but level 143 <= 192
    ASSERT_EQ(1, grids_accent(0, 0, 0, 255, 12));  // fires, level 253 > 192
    ASSERT_EQ(0,
              grids_accent(0, 0, 0, 0, 12));  // level 253 but density gate off
    PASS();
}

SUITE(grids_helpers_suite) {
    log_init();
    RUN_TEST(test_grids_level_known_points);
    RUN_TEST(test_grids_level_clamp_and_wrap);
    RUN_TEST(test_grids_trigger_density);
    RUN_TEST(test_grids_accent);
}
