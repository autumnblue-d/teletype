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

// beta_shape_icdf is the variable-shape surrogate (BETA.K). Same structural
// approach: it uses a ~1% pow approximation, so tolerances are loose.

// range + monotonicity across a sweep of shapes
TEST test_beta_shape_range_monotonic() {
    float shapes[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
    for (int s = 0; s < 5; s++) {
        float a = shapes[s];
        for (int t = 0; t < 5; t++) {
            float b = shapes[t];
            float prev = beta_shape_icdf(0.0f, a, b);
            for (int i = 0; i <= 200; i++) {
                float v = beta_shape_icdf((float)i / 200.0f, a, b);
                ASSERT(v >= 0.0f);
                ASSERT(v <= 1.0f);
                ASSERT(v >= prev - 1e-4f);  // non-decreasing
                prev = v;
            }
        }
    }
    PASS();
}

// a == b is symmetric and centred (the fix vs a raw Kumaraswamy K(a,a))
TEST test_beta_shape_symmetric() {
    float shapes[] = { 0.5f, 1.0f, 3.0f };
    for (int s = 0; s < 3; s++) {
        float a = shapes[s];
        ASSERT(fabsf(beta_shape_icdf(0.5f, a, a) - 0.5f) < 1e-3f);
        for (int i = 1; i < 10; i++) {
            float u = (float)i / 10.0f;
            ASSERT(fabsf(beta_shape_icdf(u, a, a) -
                         (1.0f - beta_shape_icdf(1.0f - u, a, a))) < 2e-3f);
        }
    }
    PASS();
}

// shape: a=b=1 is the identity; a,b>1 concentrate mid; a,b<1 push to the rails
TEST test_beta_shape_family() {
    ASSERT(fabsf(beta_shape_icdf(0.25f, 1.0f, 1.0f) - 0.25f) < 2e-3f);  // flat
    ASSERT(beta_shape_icdf(0.25f, 3.0f, 3.0f) > 0.25f);   // bell pulls up
    ASSERT(beta_shape_icdf(0.75f, 3.0f, 3.0f) < 0.75f);
    ASSERT(beta_shape_icdf(0.25f, 0.5f, 0.5f) < 0.25f);   // bimodal pushes out
    ASSERT(beta_shape_icdf(0.75f, 0.5f, 0.5f) > 0.75f);
    PASS();
}

// endpoints are exact regardless of shape
TEST test_beta_shape_endpoints() {
    ASSERT(beta_shape_icdf(0.0f, 3.0f, 0.5f) == 0.0f);
    ASSERT(beta_shape_icdf(1.0f, 3.0f, 0.5f) == 1.0f);
    ASSERT(beta_shape_icdf(-0.5f, 2.0f, 2.0f) == 0.0f);  // clamps below 0
    ASSERT(beta_shape_icdf(2.0f, 2.0f, 2.0f) == 1.0f);   // clamps above 1
    PASS();
}

SUITE(beta_suite) {
    log_init();
    RUN_TEST(test_beta_range);
    RUN_TEST(test_beta_monotonic);
    RUN_TEST(test_beta_symmetric_center);
    RUN_TEST(test_beta_concentrates);
    RUN_TEST(test_beta_shape_range_monotonic);
    RUN_TEST(test_beta_shape_symmetric);
    RUN_TEST(test_beta_shape_family);
    RUN_TEST(test_beta_shape_endpoints);
}
