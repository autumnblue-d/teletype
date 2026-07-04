// Unit tests for the ported Earthsea engine (src/es_engine.c). Ansible had no
// tests; these lock in the recording / playback / voice-allocation /
// transform behavior so the port and later refactors stay faithful. The
// engine is hardware-abstract, so these link only es_engine.o.

#include <stdint.h>
#include <string.h>

#include "es_binding.h"
#include "es_engine.h"
#include "es_grid.h"
#include "greatest/greatest.h"
#include "kria_binding.h"  // kria_note_to_cv (shared pitch mapping)
#include "music.h"         // ET

// ---- recording output vtable ----

enum { EV_ON = 0, EV_OFF = 1 };
#define MAX_EV 512

typedef struct {
    uint8_t type;
    uint8_t voice;
    int16_t semi;
    uint16_t dur;
} ev_t;

static ev_t evlog[MAX_EV];
static int evn;

static void ev_reset(void) {
    evn = 0;
    memset(evlog, 0, sizeof(evlog));
}
static void rec_on(void* ctx, uint8_t voice, int16_t semi, uint16_t dur) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_ON, voice, semi, dur };
}
static void rec_off(void* ctx, uint8_t voice) {
    (void)ctx;
    if (evn < MAX_EV) evlog[evn++] = (ev_t){ EV_OFF, voice, 0, 0 };
}

static int ev_count(uint8_t type, uint8_t voice) {
    int c = 0;
    for (int i = 0; i < evn; i++)
        if (evlog[i].type == type && evlog[i].voice == voice) c++;
    return c;
}
static const ev_t* ev_last_on(void) {
    for (int i = evn - 1; i >= 0; i--)
        if (evlog[i].type == EV_ON) return &evlog[i];
    return NULL;
}

// ---- fixtures ----

static es_engine_t E;
static const es_output_t OUT = { .note_on = rec_on,
                                 .note_off = rec_off,
                                 .ctx = NULL };

static void setup(void) {
    es_engine_set_defaults(&E.cfg);
    es_engine_init(&E, &OUT);
    ev_reset();
}

// Record a 4-event phrase: (5,5) on@1000 off@1100, (6,5) on@1200 off@1300.
// Completing at t=2000 gives intervals {100, 100, 100, 700}, length 4,
// root (5,5).
static void record_phrase(void) {
    es_engine_arm(&E);
    es_engine_grid_press(&E, 5, 5, 1, false, 1000);
    es_engine_grid_press(&E, 5, 5, 0, false, 1100);
    es_engine_grid_press(&E, 6, 5, 1, false, 1200);
    es_engine_grid_press(&E, 6, 5, 0, false, 1300);
}

// ---- defaults & validation ----

TEST defaults_are_valid(void) {
    setup();
    ASSERT(es_engine_config_valid(&E.cfg));
    ASSERT_EQ(0xF, E.cfg.voices);
    ASSERT_EQ(16, E.cfg.scale);
    for (int i = 0; i < ES_NUM_PATTERNS; i++) {
        ASSERT_EQ(0, E.cfg.p[i].length);
        ASSERT_EQ(ES_EDGE_PATTERN, E.cfg.p[i].edge);
        ASSERT_EQ(16, E.cfg.p[i].edge_time);
        ASSERT_EQ(0xF, E.cfg.p[i].voices);
        ASSERT_EQ(15, E.cfg.p[i].end);
    }
    PASS();
}

TEST validation_rejects_stale_flash(void) {
    setup();
    E.cfg.p_select = ES_NUM_PATTERNS;
    ASSERT_FALSE(es_engine_config_valid(&E.cfg));
    setup();
    E.cfg.keymap[7] = 3;
    ASSERT_FALSE(es_engine_config_valid(&E.cfg));
    setup();
    E.cfg.p[3].edge = 5;
    ASSERT_FALSE(es_engine_config_valid(&E.cfg));
    setup();
    E.cfg.p[0].length = ES_EVENTS_PER_PATTERN + 1;
    ASSERT_FALSE(es_engine_config_valid(&E.cfg));
    setup();
    E.cfg.p[0].length = 1;
    E.cfg.p[0].e[0].on = 4;
    ASSERT_FALSE(es_engine_config_valid(&E.cfg));
    PASS();
}

// ---- pitch layout ----

TEST note_index_fourths_layout(void) {
    // semitone = x + (7-y)*5 - 1
    ASSERT_EQ(0, es_engine_note_index(1, 7));    // bottom-left playable
    ASSERT_EQ(5, es_engine_note_index(1, 6));    // one row up = +5 (a fourth)
    ASSERT_EQ(1, es_engine_note_index(2, 7));    // one column right = +1
    ASSERT_EQ(14, es_engine_note_index(15, 7));  // bottom-right
    ASSERT_EQ(35, es_engine_note_index(1, 0));   // top-left playable
    ASSERT_EQ(0, es_engine_note_index(0, 7));    // clamp low (would be -1)
    ASSERT_EQ(119, es_engine_note_index(15, -15));  // clamp high (would be 124)
    PASS();
}

// ---- live keyboard & voices ----

TEST live_note_on_off(void) {
    setup();
    es_engine_grid_press(&E, 5, 5, 1, false, 100);
    ASSERT_EQ(1, evn);
    ASSERT_EQ(EV_ON, evlog[0].type);
    ASSERT_EQ(0, evlog[0].voice);
    ASSERT_EQ(14, evlog[0].semi);  // 5 + (7-5)*5 - 1
    ASSERT_EQ(0, evlog[0].dur);    // live notes: no fixed duration
    es_engine_grid_press(&E, 5, 5, 0, false, 200);
    ASSERT_EQ(2, evn);
    ASSERT_EQ(EV_OFF, evlog[1].type);
    ASSERT_EQ(0, evlog[1].voice);
    PASS();
}

TEST voice_allocation_and_steal(void) {
    setup();
    es_engine_grid_press(&E, 1, 7, 1, false, 10);
    es_engine_grid_press(&E, 2, 7, 1, false, 20);
    es_engine_grid_press(&E, 3, 7, 1, false, 30);
    es_engine_grid_press(&E, 4, 7, 1, false, 40);
    ASSERT_EQ(1, ev_count(EV_ON, 0));
    ASSERT_EQ(1, ev_count(EV_ON, 1));
    ASSERT_EQ(1, ev_count(EV_ON, 2));
    ASSERT_EQ(1, ev_count(EV_ON, 3));
    // 5th note steals the oldest (voice 0); a note_off precedes the note_on
    ev_reset();
    es_engine_grid_press(&E, 5, 7, 1, false, 50);
    ASSERT_EQ(1, ev_count(EV_OFF, 0));
    ASSERT_EQ(1, ev_count(EV_ON, 0));
    // re-pressing a sounding key reuses its voice, not a new one
    ev_reset();
    es_engine_grid_press(&E, 2, 7, 1, false, 60);
    ASSERT_EQ(1, ev_count(EV_ON, 1));
    ASSERT_EQ(0, ev_count(EV_ON, 2));
    PASS();
}

TEST voice_mask_restricts_allocation(void) {
    setup();
    E.cfg.voices = 0b0010;  // only voice 1
    es_engine_grid_press(&E, 1, 7, 1, false, 10);
    es_engine_grid_press(&E, 2, 7, 1, false, 20);
    ASSERT_EQ(0, ev_count(EV_ON, 0));
    ASSERT_EQ(2, ev_count(EV_ON, 1));  // both notes land on voice 1
    PASS();
}

// ---- recording ----

TEST recording_captures_events_and_intervals(void) {
    setup();
    record_phrase();
    es_pattern_t* p = &E.cfg.p[0];
    ASSERT_EQ(es_recording, E.rt.mode);
    ASSERT_EQ(4, p->length);
    ASSERT_EQ(5, p->root_x);
    ASSERT_EQ(5, p->root_y);
    ASSERT_EQ(1, p->e[0].on);
    ASSERT_EQ(0, p->e[1].on);
    ASSERT_EQ(5 + (5 << 4), p->e[0].index);
    ASSERT_EQ(100, p->e[0].interval);
    ASSERT_EQ(100, p->e[1].interval);
    ASSERT_EQ(100, p->e[2].interval);
    // completing (via start_playback) stamps the final interval
    es_engine_start_playback(&E, 0, 2000);
    ASSERT_EQ(700, p->e[3].interval);
    ASSERT_EQ(0, p->interval_ind);  // first supra-threshold interval
    PASS();
}

TEST recording_rest_key(void) {
    setup();
    es_engine_arm(&E);
    es_engine_grid_press(&E, 5, 5, 1, false, 1000);
    ev_reset();
    es_engine_grid_press(&E, 15, 0, 1, false, 1100);  // rest press
    es_engine_grid_press(&E, 15, 0, 0, false, 1200);
    es_pattern_t* p = &E.cfg.p[0];
    ASSERT_EQ(3, p->length);
    ASSERT_EQ(3, p->e[1].on);  // rest-on
    ASSERT_EQ(2, p->e[2].on);  // rest-off
    ASSERT_EQ(0, evn);         // rests don't sound
    PASS();
}

// ---- playback ----

TEST playback_intervals_and_loop_end(void) {
    setup();
    record_phrase();
    ev_reset();
    // non-looping: plays through once then stops
    uint32_t iv = es_engine_start_playback(&E, 0, 2000);
    ASSERT_EQ(100, iv);  // e[0].interval
    ASSERT_EQ(es_playing, E.rt.mode);
    ASSERT_EQ(1, ev_count(EV_ON, 0));  // e[0] emitted immediately
    iv = es_engine_play_advance(&E, 2100);  // e[1]: note off
    ASSERT_EQ(100, iv);
    ASSERT_EQ(1, ev_count(EV_OFF, 0));
    iv = es_engine_play_advance(&E, 2200);  // e[2]: note on
    ASSERT_EQ(100, iv);
    iv = es_engine_play_advance(&E, 2300);  // e[3]: note off
    ASSERT_EQ(700, iv);
    iv = es_engine_play_advance(&E, 3000);  // wraps; non-loop -> ends
    ASSERT_EQ(0, iv);
    ASSERT_EQ(es_stopped, E.rt.mode);
    PASS();
}

TEST playback_loops_when_loop_set(void) {
    setup();
    record_phrase();
    E.cfg.p[0].loop = 1;
    es_engine_start_playback(&E, 0, 2000);
    es_engine_play_advance(&E, 2100);
    es_engine_play_advance(&E, 2200);
    es_engine_play_advance(&E, 2300);
    ev_reset();
    uint32_t iv = es_engine_play_advance(&E, 3000);  // wrap to e[0]
    ASSERT_EQ(100, iv);
    ASSERT_EQ(es_playing, E.rt.mode);
    ASSERT_EQ(0, E.rt.pos);
    ASSERT_EQ(3000, E.rt.p_start);  // wrap re-stamps pattern start
    ASSERT_EQ(1, ev_count(EV_ON, 0));
    PASS();
}

TEST playback_transposes_from_root(void) {
    setup();
    record_phrase();
    E.cfg.p[0].root_x = 7;  // recorded root was (5,5); +2 columns = +2 semi
    es_engine_start_playback(&E, 0, 2000);
    const ev_t* on = ev_last_on();
    ASSERT(on);
    ASSERT_EQ(16, on->semi);  // 14 + 2
    PASS();
}

TEST scrub_start_positions_mid_pattern(void) {
    setup();
    record_phrase();
    E.cfg.p[0].loop = 1;
    es_engine_start_playback(&E, 0, 2000);  // computes p_total = 1000
    ASSERT_EQ(1000, E.rt.p_total);
    es_engine_stop_playback(&E);
    ev_reset();
    // pos 8/16 -> start offset 500; events cover [0,100,200,300,1000)
    uint32_t iv = es_engine_start_playback(&E, 8, 5000);
    ASSERT_EQ(3, E.rt.pos);    // event 3 spans 300..1000
    ASSERT_EQ(500, iv);        // remaining part of its interval
    ASSERT_EQ(0, evn);         // scrub start: no immediate note
    ASSERT_EQ(4500, E.rt.p_start);  // now - offset
    PASS();
}

TEST external_clock_steps_chord_groups(void) {
    setup();
    // record a chord: two presses 10 ticks apart (sub-threshold), then a gap
    es_engine_arm(&E);
    es_engine_grid_press(&E, 5, 5, 1, false, 1000);
    es_engine_grid_press(&E, 8, 5, 1, false, 1010);
    es_engine_grid_press(&E, 5, 5, 0, false, 1500);
    es_engine_grid_press(&E, 8, 5, 0, false, 1510);
    E.rt.clock_external = 1;
    uint32_t iv = es_engine_start_playback(&E, 0, 2000);
    ASSERT_EQ(0, iv);  // external clock: no timer to arm
    ASSERT_EQ(es_playing, E.rt.mode);
    E.cfg.p[0].loop = 1;
    ev_reset();
    es_engine_clock_step(&E, 2100);  // both chord notes in one step
    ASSERT_EQ(2, ev_count(EV_ON, 0) + ev_count(EV_ON, 1));
    ev_reset();
    es_engine_clock_step(&E, 2200);  // the two note-offs (also chorded)
    ASSERT_EQ(2, evn);
    PASS();
}

// ---- edge modes ----

TEST edge_fixed_sets_note_duration(void) {
    setup();
    record_phrase();
    es_engine_set_edge(&E, ES_EDGE_FIXED, 100);
    ASSERT_EQ(100, E.cfg.p[0].edge_time);
    ev_reset();
    es_engine_start_playback(&E, 0, 2000);
    const ev_t* on = ev_last_on();
    ASSERT(on);
    ASSERT_EQ(100, on->dur);  // playback notes carry the fixed duration
    PASS();
}

TEST edge_fixed_time_clamped(void) {
    setup();
    es_engine_set_edge(&E, ES_EDGE_FIXED, 1000);
    ASSERT_EQ(256, E.cfg.p[0].edge_time);
    es_engine_set_edge(&E, ES_EDGE_FIXED, 2);
    ASSERT_EQ(16, E.cfg.p[0].edge_time);
    PASS();
}

TEST edge_drone_toggles(void) {
    setup();
    E.cfg.p[0].edge = ES_EDGE_DRONE;
    es_engine_grid_press(&E, 5, 5, 1, false, 100);
    ASSERT_EQ(1, ev_count(EV_ON, 0));
    es_engine_grid_press(&E, 5, 5, 0, false, 150);  // release: ignored
    ASSERT_EQ(0, ev_count(EV_OFF, 0));
    es_engine_grid_press(&E, 5, 5, 1, false, 200);  // re-press: toggles off
    ASSERT_EQ(1, ev_count(EV_OFF, 0));
    PASS();
}

// ---- transforms ----

TEST double_and_half_speed(void) {
    setup();
    record_phrase();
    es_engine_start_playback(&E, 0, 2000);  // completes recording
    es_engine_stop_playback(&E);
    es_pattern_t* p = &E.cfg.p[0];
    es_engine_double_speed(&E);
    ASSERT_EQ(50, p->e[0].interval);   // 100 > 2*30 -> halved
    ASSERT_EQ(350, p->e[3].interval);  // 700 -> 350
    es_engine_double_speed(&E);
    ASSERT_EQ(ES_CHORD_THRESHOLD + 1, p->e[0].interval);  // 50 < 60 -> floor
    es_engine_half_speed(&E);
    ASSERT_EQ((ES_CHORD_THRESHOLD + 1) * 2, p->e[0].interval);
    PASS();
}

TEST linearize_normalizes_intervals(void) {
    setup();
    // chord (10 apart) then long gap
    es_engine_arm(&E);
    es_engine_grid_press(&E, 5, 5, 1, false, 1000);
    es_engine_grid_press(&E, 8, 5, 1, false, 1010);
    es_engine_grid_press(&E, 5, 5, 0, false, 1500);
    es_engine_start_playback(&E, 0, 2000);  // completes: intervals {10,490,500}
    es_engine_stop_playback(&E);
    es_engine_set_linearize(&E, 1);
    // reference = e[interval_ind=1].interval = 490
    ASSERT_EQ(1, E.cfg.p[0].interval_ind);
    ASSERT_EQ(1 + 490 + 490, E.rt.p_total);
    ev_reset();
    uint32_t iv = es_engine_start_playback(&E, 0, 3000);
    ASSERT_EQ(1, iv);  // sub-chord gap -> 1 tick
    iv = es_engine_play_advance(&E, 3001);
    ASSERT_EQ(490, iv);
    PASS();
}

TEST reverse_flips_events(void) {
    setup();
    record_phrase();
    es_engine_start_playback(&E, 0, 2000);
    es_engine_stop_playback(&E);
    es_pattern_t* p = &E.cfg.p[0];
    // forward: on(5,5) off(5,5) on(6,5) off(6,5), intervals {100,100,100,700}
    es_engine_set_direction(&E, 1);
    ASSERT_EQ(1, p->dir);
    // reversed: each event flips on<->off and the order reverses:
    // on(6,5) off(6,5) on(5,5) off(5,5)
    ASSERT_EQ(1, p->e[0].on);
    ASSERT_EQ(6 + (5 << 4), p->e[0].index);
    ASSERT_EQ(0, p->e[1].on);
    ASSERT_EQ(6 + (5 << 4), p->e[1].index);
    ASSERT_EQ(1, p->e[2].on);
    ASSERT_EQ(5 + (5 << 4), p->e[2].index);
    ASSERT_EQ(0, p->e[3].on);
    // reversing back restores the original
    es_engine_set_direction(&E, 0);
    ASSERT_EQ(0, p->dir);
    ASSERT_EQ(1, p->e[0].on);
    ASSERT_EQ(5 + (5 << 4), p->e[0].index);
    ASSERT_EQ(100, p->e[0].interval);
    PASS();
}

TEST transpose_walks_fourths_layout(void) {
    setup();
    record_phrase();  // root (5,5)
    es_pattern_t* p = &E.cfg.p[0];
    es_engine_transpose(&E, 2);
    ASSERT_EQ(7, p->root_x);
    ASSERT_EQ(5, p->root_y);
    es_engine_transpose(&E, -2);
    ASSERT_EQ(5, p->root_x);
    // column wrap going up: x hits 16 -> (11, y-1)
    p->root_x = 15;
    p->root_y = 5;
    es_engine_transpose(&E, 1);
    ASSERT_EQ(11, p->root_x);
    ASSERT_EQ(4, p->root_y);
    // hard bounds
    p->root_x = 15;
    p->root_y = 1;
    es_engine_transpose(&E, 3);
    ASSERT_EQ(15, p->root_x);
    ASSERT_EQ(1, p->root_y);
    p->root_x = 1;
    p->root_y = 7;
    es_engine_transpose(&E, -3);
    ASSERT_EQ(1, p->root_x);
    ASSERT_EQ(7, p->root_y);
    PASS();
}

// ---- arp & keymap ----

TEST arp_press_reroots_and_restarts(void) {
    setup();
    record_phrase();
    es_engine_start_playback(&E, 0, 2000);  // complete + play
    E.cfg.arp = 1;
    ev_reset();
    uint32_t iv = es_engine_grid_press(&E, 8, 5, 1, false, 3000);
    ASSERT_EQ(100, iv);  // playback restarted, first interval returned
    ASSERT_EQ(8, E.cfg.p[0].root_x);
    ASSERT_EQ(5, E.cfg.p[0].root_y);
    const ev_t* on = ev_last_on();
    ASSERT(on);
    ASSERT_EQ(17, on->semi);  // 14 + 3 columns
    PASS();
}

TEST keymap_cycles_when_stopped_with_rest_held(void) {
    setup();
    uint8_t i = (5 << 4) + 5;
    es_engine_grid_press(&E, 5, 5, 1, true, 100);
    ASSERT_EQ(1, E.cfg.keymap[i]);
    ASSERT_EQ(0, evn);  // keymap editing doesn't sound
    es_engine_grid_press(&E, 5, 5, 1, true, 200);
    ASSERT_EQ(2, E.cfg.keymap[i]);
    es_engine_grid_press(&E, 5, 5, 1, true, 300);
    ASSERT_EQ(0, E.cfg.keymap[i]);
    PASS();
}

// ---- transport odds and ends ----

TEST arm_and_stop_recording(void) {
    setup();
    es_engine_arm(&E);
    ASSERT_EQ(es_armed, E.rt.mode);
    es_engine_stop_recording(&E, 100);  // disarm without recording
    ASSERT_EQ(es_stopped, E.rt.mode);
    record_phrase();
    ASSERT_EQ(es_recording, E.rt.mode);
    es_engine_stop_recording(&E, 2000);
    ASSERT_EQ(es_stopped, E.rt.mode);
    ASSERT_EQ(700, E.cfg.p[0].e[3].interval);  // completion stamped
    PASS();
}

TEST stop_playback_kills_only_pattern_notes(void) {
    setup();
    record_phrase();
    E.cfg.p[0].loop = 1;
    es_engine_start_playback(&E, 0, 2000);  // pattern note on voice 0
    // live note on top (voice 1: voice 0 busy)
    es_engine_grid_press(&E, 9, 5, 1, false, 2050);
    ev_reset();
    es_engine_stop_playback(&E);
    ASSERT_EQ(1, ev_count(EV_OFF, 0));  // pattern voice killed
    ASSERT_EQ(0, ev_count(EV_OFF, 1));  // live voice survives
    ASSERT(E.rt.notes[1].active);
    PASS();
}

// ---- grid surface ----

static es_grid_state_t G;

static void gsetup(void) {
    setup();
    es_grid_state_init(&G);
}

TEST grid_start_stop_key(void) {
    gsetup();
    record_phrase();
    es_grid_process_key(&E, &G, 0, 2, 1, 100);  // press arm: stop recording
    es_grid_process_key(&E, &G, 0, 2, 0, 150);
    ASSERT_EQ(es_stopped, E.rt.mode);
    // start key: begins playback, returns the first interval for the shell
    uint32_t iv = es_grid_process_key(&E, &G, 0, 0, 1, 2000);
    ASSERT_EQ(100, iv);
    ASSERT_EQ(es_playing, E.rt.mode);
    // again: stops
    iv = es_grid_process_key(&E, &G, 0, 0, 1, 3000);
    ASSERT_EQ(0, iv);
    ASSERT_EQ(es_stopped, E.rt.mode);
    PASS();
}

TEST grid_arm_gesture(void) {
    gsetup();
    // press+release arm from stopped: arms on release
    es_grid_process_key(&E, &G, 0, 2, 1, 100);
    ASSERT_EQ(es_stopped, E.rt.mode);
    es_grid_process_key(&E, &G, 0, 2, 0, 150);
    ASSERT_EQ(es_armed, E.rt.mode);
    // press while armed: disarms, release ignored
    es_grid_process_key(&E, &G, 0, 2, 1, 200);
    ASSERT_EQ(es_stopped, E.rt.mode);
    es_grid_process_key(&E, &G, 0, 2, 0, 250);
    ASSERT_EQ(es_stopped, E.rt.mode);
    PASS();
}

TEST grid_pattern_view_and_select(void) {
    gsetup();
    es_grid_process_key(&E, &G, 0, 1, 1, 100);  // hold pattern key
    ASSERT_EQ(ES_VIEW_PATTERNS_HELD, G.view);
    es_grid_process_key(&E, &G, 3, 3, 1, 150);  // select pattern (3-2)+(1<<2)=5
    ASSERT_EQ(5, E.cfg.p_select);
    es_grid_process_key(&E, &G, 0, 1, 0, 200);  // release: back to main
    ASSERT_EQ(ES_VIEW_MAIN, G.view);
    // lock the view: hold pattern key, press start
    es_grid_process_key(&E, &G, 0, 1, 1, 300);
    es_grid_process_key(&E, &G, 0, 0, 1, 350);
    es_grid_process_key(&E, &G, 0, 1, 0, 400);
    ASSERT_EQ(ES_VIEW_PATTERNS, G.view);
    // scale select from the strip: x8-15, y3-4
    es_grid_process_key(&E, &G, 10, 3, 1, 450);
    ASSERT_EQ(2, E.cfg.scale);
    es_grid_process_key(&E, &G, 10, 3, 1, 500);  // re-press: off
    ASSERT_EQ(16, E.cfg.scale);
    PASS();
}

TEST grid_runes_overlay(void) {
    gsetup();
    record_phrase();
    es_engine_stop_recording(&E, 2000);
    es_grid_process_key(&E, &G, 0, 6, 1, 2100);  // hold runes
    ASSERT(G.runes_held);
    es_grid_process_key(&E, &G, 3, 3, 1, 2200);  // linearize rune
    ASSERT_EQ(1, E.cfg.p[0].linearize);
    es_grid_process_key(&E, &G, 7, 3, 1, 2300);  // reverse rune
    ASSERT_EQ(1, E.cfg.p[0].dir);
    es_grid_process_key(&E, &G, 0, 6, 0, 2400);  // release
    ASSERT_FALSE(G.runes_held);
    PASS();
}

TEST grid_edge_and_voices_overlays(void) {
    gsetup();
    es_grid_process_key(&E, &G, 0, 5, 1, 100);   // hold edge
    es_grid_process_key(&E, &G, 12, 3, 1, 150);  // drone zone (x>=11)
    ASSERT_EQ(ES_EDGE_DRONE, E.cfg.p[0].edge);
    es_grid_process_key(&E, &G, 4, 7, 1, 200);  // fixed-time strip: (4+1)<<4
    ASSERT_EQ(ES_EDGE_FIXED, E.cfg.p[0].edge);
    ASSERT_EQ(80, E.cfg.p[0].edge_time);
    es_grid_process_key(&E, &G, 0, 5, 0, 250);
    es_grid_process_key(&E, &G, 0, 7, 1, 300);  // hold voices
    es_grid_process_key(&E, &G, 3, 2, 1, 350);  // toggle global voice 0
    ASSERT_EQ(0xE, E.cfg.voices);
    es_grid_process_key(&E, &G, 2, 3, 1, 400);  // toggle pattern voice 1
    ASSERT_EQ(0xD, E.cfg.p[0].voices);
    es_grid_process_key(&E, &G, 0, 7, 0, 450);
    PASS();
}

TEST grid_rest_key_keymap_gesture(void) {
    gsetup();
    // hold rest (15,0), press a key while stopped: cycles keymap, no sound
    es_grid_process_key(&E, &G, 15, 0, 1, 100);
    ev_reset();
    es_grid_process_key(&E, &G, 5, 5, 1, 150);
    ASSERT_EQ(1, E.cfg.keymap[(5 << 4) + 5]);
    ASSERT_EQ(0, evn);
    es_grid_process_key(&E, &G, 15, 0, 0, 200);
    PASS();
}

TEST grid_scrub_row_while_playing(void) {
    gsetup();
    record_phrase();
    E.cfg.p[0].loop = 1;
    es_grid_process_key(&E, &G, 0, 0, 1, 2000);  // start (also completes rec)
    ASSERT_EQ(es_playing, E.rt.mode);
    uint32_t iv = es_grid_process_key(&E, &G, 8, 0, 1, 3000);  // scrub to 8/16
    ASSERT(iv > 0);
    ASSERT_EQ(3, E.rt.pos);
    PASS();
}

TEST grid_render_smoke(void) {
    gsetup();
    uint8_t led[ES_KEYMAP_SIZE];
    record_phrase();
    es_grid_refresh(&E, &G, led, 1, 2000);  // recording view
    ASSERT(led[2 << 4] >= 11);              // arm cell blinking bright
    es_grid_process_key(&E, &G, 0, 0, 1, 2000);  // play
    es_grid_refresh(&E, &G, led, 1, 2500);
    ASSERT_EQ(15, led[0]);  // start cell bright while playing
    // overlays render without touching engine state
    G.runes_held = 1;
    es_grid_refresh(&E, &G, led, 1, 2600);
    G.runes_held = 0;
    G.edge_held = 1;
    es_grid_refresh(&E, &G, led, 1, 2700);
    G.edge_held = 0;
    G.voices_held = 1;
    es_grid_refresh(&E, &G, led, 1, 2800);
    G.voices_held = 0;
    G.view = ES_VIEW_PATTERNS;
    es_grid_refresh(&E, &G, led, 1, 2900);
    ASSERT_EQ(15, led[(2 << 4) + 2]);  // selected pattern 0 cell
    PASS();
}

// ---- binding ----

TEST binding_vtable_drives_engine(void) {
    // The real binding (jacks + i2c fan-out) against the test harness's
    // tele_* / tele_ii_tx stubs: exercises the engine -> binding path
    // end-to-end without hardware.
    es_engine_set_defaults(&E.cfg);
    es_engine_init(&E, es_binding_output());
    ASSERT(E.out.note_on);
    ASSERT(E.out.note_off);
    es_engine_grid_press(&E, 5, 5, 1, false, 100);
    ASSERT(E.rt.notes[0].active);
    es_engine_grid_press(&E, 5, 5, 0, false, 200);
    ASSERT_FALSE(E.rt.notes[0].active);
    PASS();
}

TEST binding_pitch_matches_n_op(void) {
    // ES pitch goes through the same ET mapping as the N op / Kria / MP.
    ASSERT_EQ((int16_t)ET[14], kria_note_to_cv(es_engine_note_index(5, 5)));
    ASSERT_EQ((int16_t)ET[0], kria_note_to_cv(es_engine_note_index(1, 7)));
    PASS();
}

TEST empty_pattern_wont_play(void) {
    setup();
    uint32_t iv = es_engine_start_playback(&E, 0, 1000);
    ASSERT_EQ(0, iv);
    ASSERT_EQ(es_stopped, E.rt.mode);
    PASS();
}

SUITE(es_suite) {
    RUN_TEST(defaults_are_valid);
    RUN_TEST(validation_rejects_stale_flash);
    RUN_TEST(note_index_fourths_layout);
    RUN_TEST(live_note_on_off);
    RUN_TEST(voice_allocation_and_steal);
    RUN_TEST(voice_mask_restricts_allocation);
    RUN_TEST(recording_captures_events_and_intervals);
    RUN_TEST(recording_rest_key);
    RUN_TEST(playback_intervals_and_loop_end);
    RUN_TEST(playback_loops_when_loop_set);
    RUN_TEST(playback_transposes_from_root);
    RUN_TEST(scrub_start_positions_mid_pattern);
    RUN_TEST(external_clock_steps_chord_groups);
    RUN_TEST(edge_fixed_sets_note_duration);
    RUN_TEST(edge_fixed_time_clamped);
    RUN_TEST(edge_drone_toggles);
    RUN_TEST(double_and_half_speed);
    RUN_TEST(linearize_normalizes_intervals);
    RUN_TEST(reverse_flips_events);
    RUN_TEST(transpose_walks_fourths_layout);
    RUN_TEST(arp_press_reroots_and_restarts);
    RUN_TEST(keymap_cycles_when_stopped_with_rest_held);
    RUN_TEST(arm_and_stop_recording);
    RUN_TEST(stop_playback_kills_only_pattern_notes);
    RUN_TEST(grid_start_stop_key);
    RUN_TEST(grid_arm_gesture);
    RUN_TEST(grid_pattern_view_and_select);
    RUN_TEST(grid_runes_overlay);
    RUN_TEST(grid_edge_and_voices_overlays);
    RUN_TEST(grid_rest_key_keymap_gesture);
    RUN_TEST(grid_scrub_row_while_playing);
    RUN_TEST(grid_render_smoke);
    RUN_TEST(binding_vtable_drives_engine);
    RUN_TEST(binding_pitch_matches_n_op);
    RUN_TEST(empty_pattern_wont_play);
}
