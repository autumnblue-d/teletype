// Unit tests for the ported Meadowphysics engine (src/meadowphysics_engine.c).
// Ansible had no tests; these lock in the counter/rule/voice-mode behavior so
// the port and later refactors stay faithful.

#include <stdint.h>
#include <string.h>

#include "greatest/greatest.h"
#include "helpers.h"  // note_to_cv
#include "meadowphysics_binding.h"
#include "meadowphysics_clock.h"
#include "meadowphysics_engine.h"
#include "meadowphysics_grid.h"
#include "music.h"  // ET

// ---- recording output vtable ----

enum { EV_TR = 0, EV_CV = 1, EV_CVGATE = 2 };
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
static void rec_cv_gate(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_CVGATE, ch, on };
}

// count events of a given type/channel/value
static int ev_count(uint8_t type, uint8_t ch, int16_t val) {
    int c = 0;
    for (int i = 0; i < evn; i++)
        if (evlog[i].type == type && evlog[i].ch == ch && evlog[i].val == val)
            c++;
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

static mp_engine_t E;
static test_rnd_t RND;

static const mp_output_t OUT = {
    .tr = rec_tr, .cv = rec_cv, .cv_gate = rec_cv_gate, .ctx = NULL
};

// init engine, then strip all rows to inert + stopped so a single row can be
// configured in isolation.
static void isolate(void) {
    RND.next = 0;
    mp_engine_init(&E, &OUT, test_rnd, &RND);
    for (uint8_t i = 0; i < MP_ROWS; i++) {
        E.cfg.trigger[i] = 0;
        E.cfg.toggle[i] = 0;
        E.cfg.sync[i] = 0;
        E.cfg.rules[i] = MP_RULE_NONE;
        E.cfg.rule_dest_targets[i] = 0;
        E.cfg.speed[i] = 0;
    }
    mp_engine_stop(&E);  // all positions -1
    ev_reset();
}

// one clock beat = "on" edge then "off" edge
static void beat(void) {
    mp_engine_clock(&E, 1);
    mp_engine_clock(&E, 0);
}

// ---- tests ----

// Defaults are valid; out-of-range persisted config is rejected (so the mode
// falls back to defaults instead of indexing out of bounds).
TEST config_validity(void) {
    mp_config_t c;
    mp_engine_set_defaults(&c);
    ASSERT(mp_engine_config_valid(&c));
    ASSERT_EQ(MP_1V, c.voice_mode);  // set_defaults must init voice_mode/sound
    ASSERT_EQ(0, c.sound);

    c.count[3] = 200;  // off-grid column -> would index OOB
    ASSERT_FALSE(mp_engine_config_valid(&c));

    mp_engine_set_defaults(&c);
    c.rule_dests[0] = 50;  // off-array row index
    ASSERT_FALSE(mp_engine_config_valid(&c));

    mp_engine_set_defaults(&c);
    c.voice_mode = MP_SCRIPT;  // the script-trigger mode is a valid voice mode
    ASSERT(mp_engine_config_valid(&c));
    c.voice_mode = 9;
    ASSERT_FALSE(mp_engine_config_valid(&c));

    mp_engine_set_defaults(&c);
    c.speed[2] = 40;  // speed column (col-8) out of range
    ASSERT_FALSE(mp_engine_config_valid(&c));
    PASS();
}

TEST defaults_match_ansible(void) {
    mp_config_t c;
    mp_engine_set_defaults(&c);
    for (uint8_t i = 0; i < MP_ROWS; i++) {
        ASSERT_EQ(7 + i, c.count[i]);
        ASSERT_EQ(7 + i, c.min[i]);
        ASSERT_EQ(7 + i, c.max[i]);
        ASSERT_EQ((1 << i), c.trigger[i]);
        ASSERT_EQ((1 << i), c.sync[i]);
        ASSERT_EQ(MP_RULE_INC, c.rules[i]);
        ASSERT_EQ(i, c.rule_dests[i]);
        ASSERT_EQ(3, c.rule_dest_targets[i]);
    }
    PASS();
}

// A self-triggering, self-syncing row with count=C, speed=0 fires once every
// (C+1) beats.
TEST countdown_period(void) {
    isolate();
    E.cfg.count[0] = 2;
    E.cfg.trigger[0] = 1 << 0;  // trigger self
    E.cfg.sync[0] = 1 << 0;     // re-arm self
    E.cfg.voice_mode = MP_1V;
    mp_engine_reset_row(&E, 0);  // position=2, tick=0

    // beats 1,2: no fire; beat 3: fires (rising + falling edge).
    beat();
    beat();
    ASSERT_EQ(0, ev_count(EV_TR, 0, 1));
    beat();
    ASSERT_EQ(1, ev_count(EV_TR, 0, 1));  // note-on on the on-edge
    ASSERT_EQ(1, ev_count(EV_TR, 0, 0));  // note-off on the off-edge

    // over 9 more beats -> 3 more fires (period 3).
    for (int i = 0; i < 9; i++) beat();
    ASSERT_EQ(4, ev_count(EV_TR, 0, 1));
    PASS();
}

// speed acts as a divider: count=0, speed=S fires every (S+1) beats.
TEST speed_divides(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.speed[0] = 1;
    E.cfg.trigger[0] = 1 << 0;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.voice_mode = MP_1V;
    // reset_row arms position=count=0, tick=speed=1 (II_MP_RESET semantics)
    mp_engine_reset_row(&E, 0);

    // period = (count+1)*(speed+1) = 2 beats; over 6 beats -> 3 fires.
    for (int i = 0; i < 6; i++) beat();
    ASSERT_EQ(3, ev_count(EV_TR, 0, 1));
    // sanity: speed=0 on the same row fires twice as often over the same
    // window.
    ev_reset();
    E.cfg.speed[0] = 0;
    mp_engine_reset_row(&E, 0);
    for (int i = 0; i < 6; i++) beat();
    ASSERT_EQ(6, ev_count(EV_TR, 0, 1));
    PASS();
}

// INC rule bumps a destination row's count, wrapping min..max.
TEST rule_inc_wraps(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_INC;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;  // row 0 re-arms so it fires every beat
    E.cfg.min[1] = 2;
    E.cfg.max[1] = 4;
    E.cfg.count[1] = 2;  // dest starts at min
    mp_engine_reset_row(&E, 0);

    beat();
    ASSERT_EQ(3, E.cfg.count[1]);
    beat();
    ASSERT_EQ(4, E.cfg.count[1]);
    beat();
    ASSERT_EQ(2, E.cfg.count[1]);  // 5 > max -> wrap to min
    beat();
    ASSERT_EQ(3, E.cfg.count[1]);
    PASS();
}

// DEC rule wraps the other way (below min -> max).
TEST rule_dec_wraps(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_DEC;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.min[1] = 2;
    E.cfg.max[1] = 4;
    E.cfg.count[1] = 2;
    mp_engine_reset_row(&E, 0);

    beat();
    ASSERT_EQ(4, E.cfg.count[1]);  // 2-1 below min -> wrap to max
    beat();
    ASSERT_EQ(3, E.cfg.count[1]);
    PASS();
}

// MAX / MIN rules snap the destination to the range ends.
TEST rule_max_min(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_MAX;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.min[1] = 2;
    E.cfg.max[1] = 9;
    E.cfg.count[1] = 5;
    mp_engine_reset_row(&E, 0);
    beat();
    ASSERT_EQ(9, E.cfg.count[1]);

    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_MIN;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.min[1] = 2;
    E.cfg.max[1] = 9;
    E.cfg.count[1] = 5;
    mp_engine_reset_row(&E, 0);
    beat();
    ASSERT_EQ(2, E.cfg.count[1]);
    PASS();
}

// RND rule lands within min..max using the injected RNG.
TEST rule_rnd_in_range(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_RND;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.min[1] = 3;
    E.cfg.max[1] = 7;  // span = 5
    mp_engine_reset_row(&E, 0);

    RND.next = 8;  // 8 % 5 = 3 -> 3 + min(3) = 6
    beat();
    ASSERT_EQ(6, E.cfg.count[1]);
    RND.next = 5;  // 5 % 5 = 0 -> min
    beat();
    ASSERT_EQ(3, E.cfg.count[1]);
    PASS();
}

// STOP rule parks the destination row (position = -1).
TEST rule_stop(void) {
    isolate();
    E.cfg.count[0] = 0;
    E.cfg.rules[0] = MP_RULE_STOP;
    E.cfg.rule_dests[0] = 1;
    E.cfg.rule_dest_targets[0] = MP_TARGET_COUNT;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.count[1] = 3;
    mp_engine_reset_row(&E, 1);  // row 1 running
    mp_engine_reset_row(&E, 0);
    ASSERT(E.rt.position[1] >= 0);
    beat();
    ASSERT_EQ(-1, E.rt.position[1]);
    PASS();
}

// 8T voice mode: rows 0-3 drive TR, rows 4-7 drive CV-as-gate (ch = row-4).
TEST voice_8t_routing(void) {
    isolate();
    E.cfg.voice_mode = MP_8T;
    E.cfg.trigger[2] = 1 << 2;  // row 2 triggers itself
    E.cfg.trigger[5] = 1 << 5;  // row 5 triggers itself
    mp_engine_push(&E, 2);
    mp_engine_push(&E, 5);

    mp_engine_clock(&E, 1);
    ASSERT_EQ(1, ev_count(EV_TR, 2, 1));      // row 2 -> TR ch 2
    ASSERT_EQ(1, ev_count(EV_CVGATE, 1, 1));  // row 5 -> CV gate ch 1
    ASSERT_EQ(0, ev_count(EV_TR, 5, 1));      // never a TR ch 5
    mp_engine_clock(&E, 0);
    ASSERT_EQ(1, ev_count(EV_TR, 2, 0));
    ASSERT_EQ(1, ev_count(EV_CVGATE, 1, 0));
    PASS();
}

// SCRIPT voice mode: all 8 rows route to the tr seam using the full row index
// (0-7), with no cv_gate remapping -- the binding turns each into run_script.
TEST voice_script_routing(void) {
    isolate();
    E.cfg.voice_mode = MP_SCRIPT;
    E.cfg.trigger[2] = 1 << 2;  // row 2 triggers itself
    E.cfg.trigger[5] = 1 << 5;  // row 5 triggers itself
    mp_engine_push(&E, 2);
    mp_engine_push(&E, 5);

    mp_engine_clock(&E, 1);
    ASSERT_EQ(1, ev_count(EV_TR, 2, 1));      // row 2 -> tr ch 2 (not cv_gate)
    ASSERT_EQ(1, ev_count(EV_TR, 5, 1));      // row 5 -> tr ch 5 (full index)
    ASSERT_EQ(0, ev_count(EV_CVGATE, 1, 1));  // never the 8T cv_gate remap
    mp_engine_clock(&E, 0);
    ASSERT_EQ(1, ev_count(EV_TR, 2, 0));
    ASSERT_EQ(1, ev_count(EV_TR, 5, 0));
    PASS();
}

// 1V voice mode emits a CV note with the current scale degree.
TEST voice_1v_emits_cv(void) {
    isolate();
    E.cfg.voice_mode = MP_1V;
    uint8_t intervals[8] = { 0, 2, 2, 1, 2, 2, 2, 1 };
    mp_engine_calc_scale(&E, intervals);  // cur_scale = 0,2,4,5,7,9,11,12
    E.cfg.trigger[0] = 1 << 0;  // row 0 -> note uses cur_scale[7-0]=12
    mp_engine_push(&E, 0);

    mp_engine_clock(&E, 1);
    ASSERT_EQ(1, ev_count(EV_CV, 0, 12));
    ASSERT_EQ(1, ev_count(EV_TR, 0, 1));
    PASS();
}

TEST calc_scale_accumulates(void) {
    isolate();
    uint8_t intervals[8] = { 0, 2, 2, 1, 2, 2, 2, 1 };
    mp_engine_calc_scale(&E, intervals);
    uint8_t expect[8] = { 0, 2, 4, 5, 7, 9, 11, 12 };
    for (int i = 0; i < 8; i++) ASSERT_EQ(expect[i], E.rt.cur_scale[i]);
    PASS();
}

// Pitch binding converts a scale degree to a CV value via ET (TT tuning).
TEST binding_note_to_cv(void) {
    ASSERT_EQ(0, note_to_cv(0));
    ASSERT_EQ((int16_t)ET[12], note_to_cv(12));    // one octave
    ASSERT_EQ((int16_t)ET[7], note_to_cv(7));      // a fifth
    ASSERT_EQ(-(int16_t)ET[12], note_to_cv(-12));  // an octave down
    // clamped to +/-127
    ASSERT_EQ((int16_t)ET[127], note_to_cv(200));
    ASSERT_EQ(-(int16_t)ET[127], note_to_cv(-200));
    PASS();
}

// 8T full-scale gate constant is a valid 14-bit value the DAC/tele_cv accepts.
TEST binding_gate_constant(void) {
    ASSERT_EQ(16383, MP_CV_FULL);
    ASSERT(mp_binding_output() != NULL);
    ASSERT(mp_binding_output()->tr != NULL);
    ASSERT(mp_binding_output()->cv != NULL);
    ASSERT(mp_binding_output()->cv_gate != NULL);
    PASS();
}

// A 16-wide range works with no engine change: count values up to 15 are valid.
TEST range_supports_16_steps(void) {
    isolate();
    E.cfg.count[0] = 15;  // 16-step range (0..15)
    E.cfg.trigger[0] = 1 << 0;
    E.cfg.sync[0] = 1 << 0;
    E.cfg.voice_mode = MP_1V;
    mp_engine_reset_row(&E, 0);
    ASSERT_EQ(15, E.rt.position[0]);
    // fires once every 16 beats
    for (int i = 0; i < 16; i++) beat();
    ASSERT_EQ(1, ev_count(EV_TR, 0, 1));
    for (int i = 0; i < 16; i++) beat();
    ASSERT_EQ(2, ev_count(EV_TR, 0, 1));
    PASS();
}

// Internal clock toggles phase each timer fire (Ansible's edge-per-fire model).
TEST clock_internal_toggles_phase(void) {
    grid_clock_t c;
    grid_clock_init(&c, MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX, MP_CLOCK_PERIOD_DEFAULT);
    ASSERT(!c.external);
    uint8_t p = 99;
    ASSERT_EQ(1, grid_clock_internal_fire(&c, &p));
    ASSERT_EQ(1, p);  // 0 -> 1 (on-edge)
    ASSERT_EQ(1, grid_clock_internal_fire(&c, &p));
    ASSERT_EQ(0, p);  // 1 -> 0 (off-edge)
    ASSERT_EQ(1, grid_clock_internal_fire(&c, &p));
    ASSERT_EQ(1, p);
    PASS();
}

// Only the active source advances: internal timer is suppressed in external
// mode, and Tr edges are ignored in internal mode.
TEST clock_source_arbitration(void) {
    grid_clock_t c;
    grid_clock_init(&c, MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX, MP_CLOCK_PERIOD_DEFAULT);
    uint8_t p = 99;

    // internal mode: Tr edges do nothing
    ASSERT_EQ(0, grid_clock_external_edge(&c, 1, &p));
    ASSERT_EQ(99, p);

    grid_clock_set_external(&c, true);
    // external mode: internal timer does nothing
    ASSERT_EQ(0, grid_clock_internal_fire(&c, &p));
    ASSERT_EQ(99, p);
    // ...and Tr edges drive phase from the gate level
    ASSERT_EQ(1, grid_clock_external_edge(&c, 1, &p));  // gate high -> phase 1
    ASSERT_EQ(1, p);
    ASSERT_EQ(1, grid_clock_external_edge(&c, 0, &p));  // gate low -> phase 0
    ASSERT_EQ(0, p);
    PASS();
}

// The metro source (like external) suppresses the internal timer, so the metro
// tick is the sole advance -- the mode shell drives run_clock() directly.
TEST clock_metro_suppresses_internal(void) {
    grid_clock_t c;
    grid_clock_init(&c, MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX,
                    MP_CLOCK_PERIOD_DEFAULT);
    ASSERT(!c.metro);
    c.metro = true;
    uint8_t p = 99;
    ASSERT_EQ(0, grid_clock_internal_fire(&c, &p));  // internal timer off
    ASSERT_EQ(99, p);
    PASS();
}

// Tempo model: rough/fine encode the edge interval, clamped to [MIN, MAX].
TEST clock_period_model(void) {
    ASSERT_EQ(20, mp_clock_period_from_rough_fine(0, 0));
    ASSERT_EQ(36, mp_clock_period_from_rough_fine(1, 0));
    ASSERT_EQ(25, mp_clock_period_from_rough_fine(0, 5));
    // 20 + 15*16 + 15 = 275 -> clamped to 265
    ASSERT_EQ(MP_CLOCK_PERIOD_MAX, mp_clock_period_from_rough_fine(15, 15));

    grid_clock_t c;
    grid_clock_init(&c, MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX, MP_CLOCK_PERIOD_DEFAULT);
    grid_clock_set_period(&c, 5);  // below min
    ASSERT_EQ(MP_CLOCK_PERIOD_MIN, c.period);
    grid_clock_set_period(&c, 9000);  // above max
    ASSERT_EQ(MP_CLOCK_PERIOD_MAX, c.period);
    grid_clock_set_period(&c, 100);
    ASSERT_EQ(100, c.period);
    PASS();
}

TEST stop_and_reset_row(void) {
    isolate();
    E.cfg.count[3] = 6;
    mp_engine_reset_row(&E, 3);
    ASSERT_EQ(6, E.rt.position[3]);
    mp_engine_stop_row(&E, 3);
    ASSERT_EQ(-1, E.rt.position[3]);
    mp_engine_reset_row(&E, 3);
    ASSERT_EQ(6, E.rt.position[3]);
    PASS();
}

// Grid positions view: first press sets count/min/max, second press (row held)
// sets the range around count.
TEST grid_positions_press_and_range(void) {
    isolate();
    mp_grid_state_t g;
    mp_grid_state_init(&g);

    mp_grid_process_key(&E, &g, 5, 2, 1);  // first press: count=5
    ASSERT_EQ(5, E.cfg.count[2]);
    ASSERT_EQ(5, E.cfg.min[2]);
    ASSERT_EQ(5, E.cfg.max[2]);
    ASSERT_EQ(5, E.rt.position[2]);

    mp_grid_process_key(&E, &g, 9, 2, 1);  // second press (held): range up
    ASSERT_EQ(5, E.cfg.min[2]);
    ASSERT_EQ(9, E.cfg.max[2]);

    // release both, press-and-range downward
    mp_grid_process_key(&E, &g, 5, 2, 0);
    mp_grid_process_key(&E, &g, 9, 2, 0);
    mp_grid_process_key(&E, &g, 6, 2, 1);  // count=6
    mp_grid_process_key(&E, &g, 2, 2, 1);  // 2 < 6 -> min=2, max=6
    ASSERT_EQ(2, E.cfg.min[2]);
    ASSERT_EQ(6, E.cfg.max[2]);
    PASS();
}

// Holding column 0 enters speed sub-mode; then column 1 enters rules; releases
// walk back; releasing column 0 returns to positions.
TEST grid_submode_switching(void) {
    isolate();
    mp_grid_state_t g;
    mp_grid_state_init(&g);
    ASSERT_EQ(MP_GRID_POSITIONS, g.edit_mode);

    mp_grid_process_key(&E, &g, 0, 3, 1);  // hold col 0 -> speed, edit_row=3
    ASSERT_EQ(MP_GRID_SPEED, g.edit_mode);
    ASSERT_EQ(3, g.edit_row);

    mp_grid_process_key(&E, &g, 1, 5, 1);  // col 1 -> rules, edit_row=5
    ASSERT_EQ(MP_GRID_RULES, g.edit_mode);
    ASSERT_EQ(5, g.edit_row);

    mp_grid_process_key(&E, &g, 1, 5, 0);  // release col 1 -> back to speed
    ASSERT_EQ(MP_GRID_SPEED, g.edit_mode);

    mp_grid_process_key(&E, &g, 0, 3, 0);  // release col 0 -> positions
    ASSERT_EQ(MP_GRID_POSITIONS, g.edit_mode);
    PASS();
}

// Speed sub-mode: trigger/toggle are mutually exclusive; speed set; stop/start.
TEST grid_speed_edits(void) {
    isolate();
    mp_grid_state_t g;
    mp_grid_state_init(&g);
    E.cfg.trigger[0] = 0;
    E.cfg.toggle[0] = 0;

    mp_grid_process_key(&E, &g, 0, 0, 1);  // enter speed, edit_row=0

    mp_grid_process_key(&E, &g, 6, 3, 1);  // trigger bit row 3
    ASSERT(E.cfg.trigger[0] & (1 << 3));
    mp_grid_process_key(&E, &g, 5, 3, 1);  // toggle bit row 3 -> clears trigger
    ASSERT(E.cfg.toggle[0] & (1 << 3));
    ASSERT_FALSE(E.cfg.trigger[0] & (1 << 3));

    mp_grid_process_key(&E, &g, 11, 2, 1);  // speed = 11-8 = 3 on row 2
    ASSERT_EQ(3, E.cfg.speed[2]);

    E.rt.position[4] = 5;
    mp_grid_process_key(&E, &g, 2, 4, 1);  // stop row 4
    ASSERT_EQ(-1, E.rt.position[4]);
    PASS();
}

// Rules sub-mode: select rule and destination/target for edit_row.
TEST grid_rules_edits(void) {
    isolate();
    mp_grid_state_t g;
    mp_grid_state_init(&g);
    mp_grid_process_key(&E, &g, 0, 0, 1);  // speed, edit_row=0
    mp_grid_process_key(&E, &g, 1, 0, 1);  // rules, edit_row=0

    mp_grid_process_key(&E, &g, 12, 5, 1);  // rule select -> rules[0]=5
    ASSERT_EQ(5, E.cfg.rules[0]);
    mp_grid_process_key(&E, &g, 5, 3, 1);  // dest=3, target=5-3=2
    ASSERT_EQ(3, E.cfg.rule_dests[0]);
    ASSERT_EQ(2, E.cfg.rule_dest_targets[0]);
    PASS();
}

// Positions render: min..max dim, count medium, position bright; mono fallback.
TEST grid_refresh_positions(void) {
    isolate();
    mp_grid_state_t g;
    mp_grid_state_init(&g);
    E.cfg.min[0] = 1;
    E.cfg.max[0] = 5;
    E.cfg.count[0] = 3;
    E.rt.position[0] = 3;

    uint8_t led[MP_ROWS * 16];
    mp_grid_refresh(&E, &g, led, 1);  // varibright
    ASSERT_EQ(4, led[1]);             // min..max dim
    ASSERT_EQ(4, led[2]);
    ASSERT_EQ(12, led[3]);  // count+position overwrite -> bright
    ASSERT_EQ(4, led[5]);
    ASSERT_EQ(0, led[6]);  // outside range

    mp_grid_refresh(&E, &g, led, 0);  // mono: any lit cell -> full
    ASSERT_EQ(15, led[1]);
    ASSERT_EQ(15, led[3]);
    ASSERT_EQ(0, led[6]);
    PASS();
}

// Config-view scale editor: slot select + per-degree interval editing + render.
TEST grid_scale_editor(void) {
    isolate();
    uint8_t bank[MP_SCALE_SLOTS][8];
    memset(bank, 0, sizeof(bank));

    // slot select: rows 6-7 x cols 0-7 -> slot (y-6)*8 + x (no bank edit)
    E.cfg.scale = 0;
    ASSERT_FALSE(mp_grid_scale_key(&E, bank, 3, 6, 1));
    ASSERT_EQ(3, E.cfg.scale);
    ASSERT_FALSE(mp_grid_scale_key(&E, bank, 2, 7, 1));  // 8 + 2
    ASSERT_EQ(10, E.cfg.scale);

    // interval edit: right half, row y -> degree 7-y, value x-8 (edits bank)
    E.cfg.scale = 5;
    ASSERT(mp_grid_scale_key(&E, bank, 8 + 3, 7, 1));  // degree 0 = 3
    ASSERT_EQ(3, bank[5][0]);
    ASSERT(mp_grid_scale_key(&E, bank, 8 + 2, 0, 1));  // degree 7 = 2
    ASSERT_EQ(2, bank[5][7]);

    // release and dead zone are no-ops
    ASSERT_FALSE(mp_grid_scale_key(&E, bank, 8 + 1, 3, 0));  // z=0
    ASSERT_FALSE(mp_grid_scale_key(&E, bank, 2, 3, 1));      // x<8, y<6

    // render: current slot bright, degree intervals lit
    uint8_t led[MP_ROWS * 16];
    mp_grid_scale_refresh(&E, bank, led, 1);
    ASSERT_EQ(12, led[6 * 16 + 5]);  // slot 5 -> row 6 col 5, bright
    ASSERT_EQ(8, led[7 * 16 + 11]);  // degree 0 (val 3) -> row 7 col 11
    ASSERT_EQ(8, led[0 * 16 + 10]);  // degree 7 (val 2) -> row 0 col 10
    PASS();
}

SUITE(meadowphysics_suite) {
    RUN_TEST(config_validity);
    RUN_TEST(defaults_match_ansible);
    RUN_TEST(countdown_period);
    RUN_TEST(speed_divides);
    RUN_TEST(rule_inc_wraps);
    RUN_TEST(rule_dec_wraps);
    RUN_TEST(rule_max_min);
    RUN_TEST(rule_rnd_in_range);
    RUN_TEST(rule_stop);
    RUN_TEST(voice_8t_routing);
    RUN_TEST(voice_script_routing);
    RUN_TEST(voice_1v_emits_cv);
    RUN_TEST(calc_scale_accumulates);
    RUN_TEST(binding_note_to_cv);
    RUN_TEST(binding_gate_constant);
    RUN_TEST(range_supports_16_steps);
    RUN_TEST(clock_internal_toggles_phase);
    RUN_TEST(clock_source_arbitration);
    RUN_TEST(clock_metro_suppresses_internal);
    RUN_TEST(clock_period_model);
    RUN_TEST(grid_positions_press_and_range);
    RUN_TEST(grid_submode_switching);
    RUN_TEST(grid_speed_edits);
    RUN_TEST(grid_rules_edits);
    RUN_TEST(grid_refresh_positions);
    RUN_TEST(grid_scale_editor);
    RUN_TEST(stop_and_reset_row);
}
