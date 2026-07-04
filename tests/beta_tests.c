#include <math.h>

#include "beta.h"
#include "greatest/greatest.h"
#include "log.h"

// beta_fast is a pure inverse-CDF of a symmetric beta(3,3). Assert on
// STRUCTURE (range, monotonicity, symmetry, center-concentration) rather than
// exact float bits - host hard-float and AVR32 soft-float differ in the last
// ULP.

// output always in [0,1) across the input domain
TEST test_beta_range() {
    for (int i = 0; i <= 1000; i++) {
        float u = (float)i / 1000.0f;
        float v = beta_fast(u);
        ASSERT(v >= 0.0f);
        ASSERT(v < 1.0f);
    }
    PASS();
}

// an inverse-CDF is non-decreasing
TEST test_beta_monotonic() {
    float prev = beta_fast(0.0f);
    for (int i = 1; i <= 1000; i++) {
        float u = (float)i / 1000.0f;
        float v = beta_fast(u);
        ASSERT(v >= prev - 1e-6f);
        prev = v;
    }
    PASS();
}

// beta(3,3) is symmetric: median maps to the center
TEST test_beta_symmetric_center() {
    ASSERT(fabsf(beta_fast(0.5f) - 0.5f) < 1e-3f);
    // icdf(u) ~= 1 - icdf(1-u)
    for (int i = 1; i < 10; i++) {
        float u = (float)i / 10.0f;
        ASSERT(fabsf(beta_fast(u) - (1.0f - beta_fast(1.0f - u))) < 2e-3f);
    }
    PASS();
}

// center-concentration: percentiles are pulled toward 0.5 vs a flat mapping
TEST test_beta_concentrates() {
    ASSERT(beta_fast(0.25f) > 0.25f);  // below-median input pulled up
    ASSERT(beta_fast(0.75f) < 0.75f);  // above-median input pulled down
    ASSERT(beta_fast(0.0f) < 0.05f);   // tail reaches toward the rail
    PASS();
}

SUITE(beta_suite) {
    log_init();
    RUN_TEST(test_beta_range);
    RUN_TEST(test_beta_monotonic);
    RUN_TEST(test_beta_symmetric_center);
    RUN_TEST(test_beta_concentrates);
}
