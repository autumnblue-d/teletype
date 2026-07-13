// Unit tests for the ported Kria engine (src/kria_engine.c). Ansible had no
// tests; these lock in the stepping / loop / divider / meta / scale / pitch
// behavior so the port and later refactors stay faithful. The engine is
// hardware-abstract, so these link only kria_engine.o (no libavr32).

#include <stdint.h>
#include <string.h>

#include "greatest/greatest.h"
#include "helpers.h"  // note_to_cv
#include "kria_binding.h"
#include "kria_clock.h"
#include "kria_engine.h"
#include "kria_grid.h"
#include "music.h"  // ET

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
    kria_engine_clock(&E, 1);  // advances all params to step 0, fires
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

// ---- binding tests ----

TEST note_to_cv_matches_et(void) {
    ASSERT_EQ(0, note_to_cv(0));
    ASSERT_EQ((int16_t)ET[12], note_to_cv(12));
    ASSERT_EQ((int16_t)ET[60], note_to_cv(60));
    // negative semitones map below 0V
    ASSERT_EQ(-(int16_t)ET[12], note_to_cv(-12));
    // clamped to +/-127
    ASSERT_EQ((int16_t)ET[127], note_to_cv(200));
    ASSERT_EQ(-(int16_t)ET[127], note_to_cv(-200));
    PASS();
}

TEST binding_output_vtable_is_wired(void) {
    const kria_output_t* o = kria_binding_output();
    ASSERT(o != NULL);
    ASSERT(o->tr != NULL);
    ASSERT(o->cv != NULL);
    ASSERT(o->cv_slew != NULL);
    PASS();
}

// ---- clock tests ----

TEST clock_internal_toggles_phase(void) {
    grid_clock_t c;
    uint8_t ph = 99;
    grid_clock_init(&c, KR_CLOCK_PERIOD_MIN, KR_CLOCK_PERIOD_MAX, KR_CLOCK_PERIOD_DEFAULT);
    ASSERT_EQ(1, grid_clock_internal_fire(&c, &ph));
    ASSERT_EQ(1, ph);
    ASSERT_EQ(1, grid_clock_internal_fire(&c, &ph));
    ASSERT_EQ(0, ph);
    // external edge ignored while internal
    ASSERT_EQ(0, grid_clock_external_edge(&c, 1, &ph));
    PASS();
}

TEST clock_external_follows_level(void) {
    grid_clock_t c;
    uint8_t ph = 99;
    grid_clock_init(&c, KR_CLOCK_PERIOD_MIN, KR_CLOCK_PERIOD_MAX, KR_CLOCK_PERIOD_DEFAULT);
    grid_clock_set_external(&c, true);
    ASSERT_EQ(0, grid_clock_internal_fire(&c, &ph));  // suppressed
    ASSERT_EQ(1, grid_clock_external_edge(&c, 1, &ph));
    ASSERT_EQ(1, ph);
    ASSERT_EQ(1, grid_clock_external_edge(&c, 0, &ph));
    ASSERT_EQ(0, ph);
    PASS();
}

TEST clock_period_is_clamped(void) {
    grid_clock_t c;
    grid_clock_init(&c, KR_CLOCK_PERIOD_MIN, KR_CLOCK_PERIOD_MAX, KR_CLOCK_PERIOD_DEFAULT);
    grid_clock_set_period(&c, 5);
    ASSERT_EQ(KR_CLOCK_PERIOD_MIN, c.period);
    grid_clock_set_period(&c, 5000);
    ASSERT_EQ(KR_CLOCK_PERIOD_MAX, c.period);
    grid_clock_set_period(&c, 120);
    ASSERT_EQ(120, c.period);
    PASS();
}

TEST clock_scaling_matches_ansible(void) {
    // dur_unscaled=16, clock_delta=100, tmul_tr=1:
    //   scale = 100*1/384 = 0.2604; 16*0.2604 = 4.16 -> 4
    ASSERT_EQ(4, kria_clock_scale_duration(16, 100, 1));
    // repeat ticks = 100*1/2 = 50
    ASSERT_EQ(50, kria_clock_repeat_ticks(100, 1, 2));
    // tmul multiplies the reference length
    ASSERT_EQ(100, kria_clock_repeat_ticks(100, 2, 2));
    // rpt=0 is guarded (treated as 1)
    ASSERT_EQ(200, kria_clock_repeat_ticks(200, 1, 0));
    PASS();
}

// ---- grid tests ----

TEST grid_defaults(void) {
    kria_grid_state_t G;
    kria_grid_state_init(&G);
    ASSERT_EQ(KR_P_TR, G.mode);
    ASSERT_EQ(KR_MOD_NONE, G.mod_mode);
    ASSERT_EQ(1, G.note_sync);
    ASSERT_EQ(2, G.loop_sync);
    PASS();
}

TEST grid_bottom_row_selects_mode_and_track(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);

    kria_grid_process_key(&E, &G, 5, 7, 1);  // x5 toggles mTr<->mRpt
    ASSERT_EQ(KR_P_RPT, G.mode);
    kria_grid_process_key(&E, &G, 5, 7, 1);
    ASSERT_EQ(KR_P_TR, G.mode);
    kria_grid_process_key(&E, &G, 2, 7, 1);  // x0-3 select track
    ASSERT_EQ(2, G.track);
    PASS();
}

TEST grid_tr_page_toggles_step(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);  // mode = mTr, modNone

    // on the tr page the row y IS the track; toggle track 1 step 3
    kria_grid_process_key(&E, &G, 3, 1, 1);
    ASSERT_EQ(1, E.cfg.p[0].t[1].tr[3]);
    kria_grid_process_key(&E, &G, 3, 1, 1);
    ASSERT_EQ(0, E.cfg.p[0].t[1].tr[3]);
    PASS();
}

TEST grid_loop_gesture_sets_range(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mode = KR_P_NOTE;
    G.mod_mode = KR_MOD_LOOP;
    G.loop_sync = 0;  // this param only

    kria_grid_process_key(&E, &G, 2, 3, 1);  // first press -> loop_first
    kria_grid_process_key(&E, &G, 5, 3, 1);  // second press -> range 2..5
    ASSERT_EQ(2, E.cfg.p[0].t[0].lstart[KR_P_NOTE]);
    ASSERT_EQ(5, E.cfg.p[0].t[0].lend[KR_P_NOTE]);
    kria_grid_process_key(&E, &G, 2, 3, 0);
    kria_grid_process_key(&E, &G, 5, 3, 0);
    PASS();
}

// mPattern long-press (act-on-release): a quick press/release just switches
// pattern; the copy gesture must not fire.
TEST grid_pattern_quick_release_switches(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mode = KR_MODE_PATTERN;  // cue/meta default 0 -> plain-select path

    kria_grid_process_key(&E, &G, 3, 0, 1);  // press slot 3: deferred
    ASSERT_EQ(0, E.cfg.pattern);             // not switched on press
    ASSERT_EQ(1, G.hold_pending);
    kria_grid_process_key(&E, &G, 3, 0, 0);  // quick release: switch
    ASSERT_EQ(3, E.cfg.pattern);
    ASSERT_EQ(0, G.hold_pending);
    PASS();
}

// Holding a slot past the threshold copies the playing pattern into it, then
// switches; the eventual physical release is then a no-op.
TEST grid_pattern_hold_copies_and_switches(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    E.cfg.p[0].t[0].tr[5] = 1;  // make the playing pattern distinctive
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mode = KR_MODE_PATTERN;

    kria_grid_process_key(&E, &G, 2, 0, 1);  // press slot 2 (hold pending)
    ASSERT_EQ(0, E.cfg.pattern);
    ASSERT_EQ(0, E.cfg.p[2].t[0].tr[5]);                // not yet copied
    ASSERT_EQ(1, kria_grid_pattern_hold_fire(&E, &G));  // threshold elapsed
    ASSERT_EQ(2, E.cfg.pattern);                        // switched to the slot
    ASSERT_EQ(1, E.cfg.p[2].t[0].tr[5]);                // playing pattern copied
    ASSERT_EQ(0, G.hold_pending);
    kria_grid_process_key(&E, &G, 2, 0, 0);  // release after fire: no-op
    ASSERT_EQ(2, E.cfg.pattern);
    ASSERT_EQ(0, kria_grid_pattern_hold_fire(&E, &G));  // nothing pending
    PASS();
}

// Leaving the pattern page (a bottom-row press) abandons a pending hold so it
// cannot fire later on the wrong page.
TEST grid_pattern_hold_abandoned_on_page_change(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mode = KR_MODE_PATTERN;

    kria_grid_process_key(&E, &G, 4, 0, 1);  // press slot 4 (hold pending)
    ASSERT_EQ(1, G.hold_pending);
    kria_grid_process_key(&E, &G, 8, 7, 1);  // bottom row x8 -> mDur page
    ASSERT_EQ(0, G.hold_pending);
    ASSERT_EQ(0, kria_grid_pattern_hold_fire(&E, &G));  // won't fire
    ASSERT_EQ(0, E.cfg.pattern);
    PASS();
}

TEST grid_render_smoke(void) {
    kria_grid_state_t G;
    uint8_t led[128];
    kria_engine_set_defaults(&E.cfg);
    E.cfg.p[0].t[0].tr[3] = 1;
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);  // mTr page

    kria_grid_refresh(&E, &G, led, 1);
    ASSERT(led[0 * 16 + 3] > 0);  // the trigger cell is lit
    ASSERT(led[112 + 0] > 0);     // bottom-row track 0 select
    // every cell within range
    for (int i = 0; i < 128; i++) ASSERT(led[i] <= 15);

    // non-varibright: lit cells forced to full
    kria_grid_refresh(&E, &G, led, 0);
    ASSERT_EQ(15, led[0 * 16 + 3]);
    PASS();
}

TEST loop_setters_wrap(void) {
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_engine_set_loop_start(&E, 0, KR_P_TR, 14);
    kria_engine_set_loop_len(&E, 0, KR_P_TR, 4);  // start 14, len 4 -> wraps
    ASSERT_EQ(14, E.cfg.p[0].t[0].lstart[KR_P_TR]);
    ASSERT_EQ(4, E.cfg.p[0].t[0].llen[KR_P_TR]);
    ASSERT_EQ(1, E.cfg.p[0].t[0].lend[KR_P_TR]);   // 14+4-1 = 17 -> 1
    ASSERT_EQ(1, E.cfg.p[0].t[0].lswap[KR_P_TR]);  // wrapped
    PASS();
}

// ---- MP-style cascade sequencer on the DUR sub-tab (6 lanes -> scripts 3-8) --
// The cascade/rule engine itself is covered by meadowphysics_tests.c; these
// exercise the Kria integration: defaults, the DUR toggle, and the grid adapter
// that forwards rows 0-5 to the reused MP grid handler (row 6 / the nav row 7
// are excluded).

static void t_mp_tr(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    (void)ch;
    (void)on;
}
static void t_mp_cv(void* c, uint8_t ch, int16_t n) {
    (void)c;
    (void)ch;
    (void)n;
}
static void t_mp_cvg(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    (void)ch;
    (void)on;
}
static const mp_output_t T_MP_OUT = {
    .tr = t_mp_tr, .cv = t_mp_cv, .cv_gate = t_mp_cvg, .ctx = NULL
};

TEST mpseq_defaults_are_script_mode(void) {
    kria_engine_set_defaults(&E.cfg);
    for (uint8_t p = 0; p < KRIA_NUM_PATTERNS; p++) {
        ASSERT_EQ(MP_SCRIPT, E.cfg.p[p].mpseq.voice_mode);
        ASSERT(mp_engine_config_valid(&E.cfg.p[p].mpseq));
        // lanes 6-7 (no matching script) have their cascade masks cleared
        for (uint8_t l = KRIA_SCRIPT_LANES; l < MP_ROWS; l++) {
            ASSERT_EQ(0, E.cfg.p[p].mpseq.trigger[l]);
            ASSERT_EQ(0, E.cfg.p[p].mpseq.toggle[l]);
            ASSERT_EQ(0, E.cfg.p[p].mpseq.sync[l]);
        }
    }
    PASS();
}

TEST grid_dur_button_double_tabs_mpseq(void) {
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);  // starts on KR_P_TR
    kria_grid_process_key(&E, &G, 8, 7, 1);  // x8: -> DUR
    ASSERT_EQ(KR_P_DUR, G.mode);
    kria_grid_process_key(&E, &G, 8, 7, 1);  // x8 again: -> MPSEQ
    ASSERT_EQ(KR_MODE_MPSEQ, G.mode);
    kria_grid_process_key(&E, &G, 8, 7, 1);  // x8 again: back to DUR
    ASSERT_EQ(KR_P_DUR, G.mode);
    PASS();
}

TEST grid_mpseq_forwards_lane_rows_only(void) {
    mp_engine_t M;
    mp_engine_init(&M, &T_MP_OUT, test_rnd, &RND);
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mpseq = &M;
    G.mode = KR_MODE_MPSEQ;

    // POSITIONS view (default): a press on a lane row sets that counter's
    // count/position to the pressed column (MP's 1st-press behavior). Use x>=2
    // to avoid col0/col1 (MP's view-switch gestures).
    kria_grid_process_key(&E, &G, 3, 0, 1);
    ASSERT_EQ(3, M.cfg.count[0]);
    ASSERT_EQ(3, M.rt.position[0]);

    // Row 6 is not a lane: the press must be dropped (count[6] unchanged).
    uint8_t cnt6 = M.cfg.count[6];
    kria_grid_process_key(&E, &G, 5, 6, 1);
    ASSERT_EQ(cnt6, M.cfg.count[6]);

    // Row 7 is the nav bar, handled by Kria (not forwarded to MP): x8 toggles
    // MPSEQ back to DUR.
    kria_grid_process_key(&E, &G, 8, 7, 1);
    ASSERT_EQ(KR_P_DUR, G.mode);
    PASS();
}

TEST grid_mpseq_render_keeps_nav_and_blank_row6(void) {
    mp_engine_t M;
    mp_engine_init(&M, &T_MP_OUT, test_rnd, &RND);
    kria_grid_state_t G;
    kria_engine_set_defaults(&E.cfg);
    kria_engine_init(&E, &OUT, test_rnd, &RND, NULL);
    kria_grid_state_init(&G);
    G.mpseq = &M;
    G.mode = KR_MODE_MPSEQ;

    uint8_t led[128];
    kria_grid_refresh(&E, &G, led, 1);
    for (int i = 0; i < 128; i++) ASSERT(led[i] <= 15);
    for (int x = 0; x < 16; x++) ASSERT_EQ(0, led[6 * 16 + x]);  // row 6 blank
    ASSERT(led[7 * 16 + 8] > 0);  // row 7: shared DUR/MPSEQ selector lit
    PASS();
}

SUITE(kria_suite) {
    RUN_TEST(defaults_are_valid);
    RUN_TEST(loop_setters_wrap);
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
    RUN_TEST(note_to_cv_matches_et);
    RUN_TEST(binding_output_vtable_is_wired);
    RUN_TEST(clock_internal_toggles_phase);
    RUN_TEST(clock_external_follows_level);
    RUN_TEST(clock_period_is_clamped);
    RUN_TEST(clock_scaling_matches_ansible);
    RUN_TEST(grid_defaults);
    RUN_TEST(grid_bottom_row_selects_mode_and_track);
    RUN_TEST(grid_tr_page_toggles_step);
    RUN_TEST(grid_loop_gesture_sets_range);
    RUN_TEST(grid_pattern_quick_release_switches);
    RUN_TEST(grid_pattern_hold_copies_and_switches);
    RUN_TEST(grid_pattern_hold_abandoned_on_page_change);
    RUN_TEST(grid_render_smoke);
    RUN_TEST(mpseq_defaults_are_script_mode);
    RUN_TEST(grid_dur_button_double_tabs_mpseq);
    RUN_TEST(grid_mpseq_forwards_lane_rows_only);
    RUN_TEST(grid_mpseq_render_keeps_nav_and_blank_row6);
}
