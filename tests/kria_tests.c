// Unit tests for the ported Kria engine (src/kria_engine.c). Ansible had no
// tests; these lock in the stepping / loop / divider / meta / scale / pitch
// behavior so the port and later refactors stay faithful. The engine is
// hardware-abstract, so these link only kria_engine.o (no libavr32).

#include <stdint.h>
#include <string.h>

#include "greatest/greatest.h"
#include "kria_engine.h"

// ---- recording output vtable ----

enum { EV_TR = 0, EV_CV = 1, EV_SLEW = 2 };
#define MAX_EV 512

typedef struct {
    uint8_t type;
    uint8_t ch;
    int16_t val;
} ev_t;

static ev_t evlog[MAX_EV];
static int evn;

static void ev_reset(void) {
    evn = 0;
    memset(evlog, 0, sizeof(evlog));
}
static void rec_tr(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_TR, ch, on };
}
static void rec_cv(void* ctx, uint8_t ch, int16_t note) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_CV, ch, note };
}
static void rec_slew(void* ctx, uint8_t ch, uint16_t slew) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_SLEW, ch, (int16_t)slew };
}

static int ev_count(uint8_t type, uint8_t ch, int16_t val) {
    int c = 0;
    for (int i = 0; i < evn; i++)
        if (evlog[i].type == type && evlog[i].ch == ch && evlog[i].val == val)
            c++;
    return c;
}
static int ev_count_type_ch(uint8_t type, uint8_t ch) {
    int c = 0;
    for (int i = 0; i < evn; i++)
        if (evlog[i].type == type && evlog[i].ch == ch) c++;
    return c;
}

// ---- deterministic RNG ----

typedef struct {
    uint32_t next;
} test_rnd_t;
static uint32_t test_rnd(void* ctx) {
    return ((test_rnd_t*)ctx)->next;
}

// ---- fixtures ----

static kria_engine_t E;
static test_rnd_t RND;
static const kria_output_t OUT = {
    .tr = rec_tr, .cv = rec_cv, .cv_slew = rec_slew, .ctx = NULL
};

// major-scale intervals for calc_scale -> cur_scale = {0,2,4,5,7,9,11,12}
static const uint8_t MAJOR[8] = { 0, 2, 2, 1, 2, 2, 2, 1 };

// Set every param loop of track `t` to [lstart,lend].
static void set_loop(uint8_t t, uint8_t lstart, uint8_t lend) {
    for (uint8_t p = 0; p < KRIA_NUM_PARAMS; p++) {
        E.cfg.p[0].t[t].lstart[p] = lstart;
        E.cfg.p[0].t[t].lend[p] = lend;
    }
}
// Enable a trigger on every step of track `t`.
static void set_all_triggers(uint8_t t) {
    for (uint8_t s = 0; s < 16; s++) E.cfg.p[0].t[t].tr[s] = 1;
}

static void fixture_single_track(uint8_t lstart, uint8_t lend) {
    ev_reset();
    RND.next = 3;
    kria_engine_set_defaults(&E.cfg);
    set_loop(0, lstart, lend);
    set_all_triggers(0);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
}

static void clocks(int n) {
    for (int i = 0; i < n; i++) kria_engine_clock(&E, 1);
}

// ---- tests ----

TEST defaults_are_valid(void) {
    kria_config_t cfg;
    kria_engine_set_defaults(&cfg);
    ASSERT(kria_engine_config_valid(&cfg));
    // spot-check a few documented defaults
    ASSERT_EQ(4, cfg.p[0].t[0].dur_mul);
    ASSERT_EQ(1, cfg.p[0].t[0].tmul[KR_P_TR]);
    ASSERT_EQ(1, cfg.p[0].t[0].rpt[0]);
    ASSERT_EQ(3, cfg.p[0].t[0].p[KR_P_TR][0]);
    ASSERT_EQ(5, cfg.p[0].t[0].lend[0]);
    ASSERT_EQ(3, cfg.meta_end);
    ASSERT_EQ(7, cfg.meta_steps[0]);
    PASS();
}

TEST config_valid_rejects_bad(void) {
    kria_config_t cfg;

    kria_engine_set_defaults(&cfg);
    cfg.pattern = KRIA_NUM_PATTERNS;  // out of range
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    kria_engine_set_defaults(&cfg);
    cfg.p[0].t[0].tmul[0] = 0;  // divider must be >= 1
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    kria_engine_set_defaults(&cfg);
    cfg.p[0].t[0].rpt[3] = 0;  // repeat count must be >= 1
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    kria_engine_set_defaults(&cfg);
    cfg.p[0].t[0].direction = 9;  // > KR_DIR_RANDOM
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    kria_engine_set_defaults(&cfg);
    cfg.p[0].t[0].lend[2] = 20;  // loop index out of range
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    kria_engine_set_defaults(&cfg);
    cfg.meta_pat[5] = KRIA_NUM_PATTERNS;  // meta target out of range
    ASSERT_FALSE(kria_engine_config_valid(&cfg));

    PASS();
}

TEST calc_scale_is_cumulative(void) {
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_engine_calc_scale(&E, MAJOR);
    ASSERT_EQ(0, E.rt.cur_scale[0]);
    ASSERT_EQ(2, E.rt.cur_scale[1]);
    ASSERT_EQ(4, E.rt.cur_scale[2]);
    ASSERT_EQ(5, E.rt.cur_scale[3]);
    ASSERT_EQ(7, E.rt.cur_scale[4]);
    ASSERT_EQ(12, E.rt.cur_scale[7]);
    PASS();
}

TEST forward_stepping_fires_each_clock(void) {
    fixture_single_track(0, 3);
    clocks(4);
    // every clock advances mTr (tmul=1) onto a step whose tr=1 and p=3 -> fire
    ASSERT_EQ(4, ev_count(EV_TR, 0, 1));
    // positions cycled 0,1,2,3 -> back to 0 next; after 4 clocks pos == 3
    ASSERT_EQ(3, E.rt.pos[0][KR_P_TR]);
    // no other track fired
    ASSERT_EQ(0, ev_count_type_ch(EV_TR, 1));
    PASS();
}

TEST tmul_divides_the_clock(void) {
    fixture_single_track(0, 3);
    E.cfg.p[0].t[0].tmul[KR_P_TR] = 2;  // trigger param advances every 2 clocks
    kria_engine_reset(&E);              // re-prime pos_mul from the new tmul
    ev_reset();
    clocks(8);
    ASSERT_EQ(4, ev_count(EV_TR, 0, 1));  // 8 clocks / 2 = 4 fires
    PASS();
}

TEST loop_bounds_are_respected(void) {
    fixture_single_track(1, 2);  // loop only steps 1..2
    kria_engine_clock(&E, 1);
    ASSERT_EQ(1, E.rt.pos[0][KR_P_TR]);  // lend -> lstart
    kria_engine_clock(&E, 1);
    ASSERT_EQ(2, E.rt.pos[0][KR_P_TR]);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(1, E.rt.pos[0][KR_P_TR]);  // wraps back to lstart
    PASS();
}

TEST reverse_direction_counts_down(void) {
    fixture_single_track(0, 3);
    E.cfg.p[0].t[0].direction = KR_DIR_REVERSE;
    kria_engine_reset(&E);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(2, E.rt.pos[0][KR_P_TR]);  // lend(3) - 1
    kria_engine_clock(&E, 1);
    ASSERT_EQ(1, E.rt.pos[0][KR_P_TR]);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(0, E.rt.pos[0][KR_P_TR]);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(3, E.rt.pos[0][KR_P_TR]);  // lstart -> lend wrap
    PASS();
}

TEST random_direction_is_deterministic_with_fixed_rng(void) {
    fixture_single_track(1, 3);
    E.cfg.p[0].t[0].direction = KR_DIR_RANDOM;
    RND.next = 0;  // lstart + 0 % range = lstart
    kria_engine_reset(&E);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(1, E.rt.pos[0][KR_P_TR]);
    PASS();
}

TEST mute_suppresses_output(void) {
    fixture_single_track(0, 3);
    kria_engine_set_mute(&E, 0, 1);
    clocks(4);
    ASSERT_EQ(0, ev_count_type_ch(EV_TR, 0));
    ASSERT_EQ(0, ev_count_type_ch(EV_CV, 0));
    kria_engine_set_mute(&E, 0, 0);
    clocks(1);
    ASSERT(ev_count_type_ch(EV_TR, 0) > 0);
    PASS();
}

TEST note_maps_through_scale_and_octave(void) {
    fixture_single_track(0, 3);
    E.cfg.p[0].t[0].note[0] = 2;  // scale degree 2
    E.cfg.p[0].t[0].oct[0] = 1;   // octave 1
    kria_engine_reset(&E);
    kria_engine_calc_scale(&E, MAJOR);  // cur_scale[2] == 4
    ev_reset();
    kria_engine_clock(&E, 1);           // advances all params to step 0, fires
    // semitones = cur_scale[2] + (oct 1)*12 = 4 + 12 = 16
    ASSERT_EQ(1, ev_count(EV_CV, 0, 16));
    PASS();
}

TEST meta_sequencer_chains_patterns(void) {
    kria_engine_set_defaults(&E.cfg);
    E.cfg.meta = 1;
    E.cfg.cue_div = 0;
    E.cfg.cue_steps = 0;  // one cue step per clock
    E.cfg.meta_start = 0;
    E.cfg.meta_end = 1;
    memset(E.cfg.meta_steps, 0, 64);  // dwell 1 cue step each
    E.cfg.meta_pat[0] = 0;
    E.cfg.meta_pat[1] = 1;
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);

    ASSERT_EQ(0, E.cfg.pattern);
    kria_engine_clock(&E, 1);
    ASSERT_EQ(1, E.cfg.pattern);  // advanced 0 -> 1
    kria_engine_clock(&E, 1);
    ASSERT_EQ(0, E.cfg.pattern);  // wrapped meta_end -> meta_start
    PASS();
}

TEST falling_edge_is_ignored(void) {
    fixture_single_track(0, 3);
    kria_engine_clock(&E, 0);  // phase 0: no work
    ASSERT_EQ(0, evn);
    PASS();
}

TEST repeat_retriggers_on_set_bits(void) {
    fixture_single_track(0, 3);
    // step 0: 3 repeats, all repeat bits set (0b111)
    E.cfg.p[0].t[0].rpt[0] = 3;
    E.cfg.p[0].t[0].rptBits[0] = 0x07;
    kria_engine_reset(&E);
    ev_reset();
    kria_engine_clock(&E, 1);  // fires the first repeat (bit 0)
    int first = ev_count_type_ch(EV_TR, 0);
    ASSERT(first >= 1);
    ASSERT_EQ(2, E.rt.repeats[0]);  // rpt-1
    kria_engine_repeat(&E, 0);      // bit 1
    kria_engine_repeat(&E, 0);      // bit 2
    // two more gate-on events from the repeats
    ASSERT_EQ(first + 2, ev_count_type_ch(EV_TR, 0));
    PASS();
}

SUITE(kria_suite) {
    RUN_TEST(defaults_are_valid);
    RUN_TEST(config_valid_rejects_bad);
    RUN_TEST(calc_scale_is_cumulative);
    RUN_TEST(forward_stepping_fires_each_clock);
    RUN_TEST(tmul_divides_the_clock);
    RUN_TEST(loop_bounds_are_respected);
    RUN_TEST(reverse_direction_counts_down);
    RUN_TEST(random_direction_is_deterministic_with_fixed_rng);
    RUN_TEST(mute_suppresses_output);
    RUN_TEST(note_maps_through_scale_and_octave);
    RUN_TEST(meta_sequencer_chains_patterns);
    RUN_TEST(falling_edge_is_ignored);
    RUN_TEST(repeat_retriggers_on_set_bits);
}
