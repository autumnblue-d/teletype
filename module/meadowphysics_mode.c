#include "meadowphysics_mode.h"

#include <string.h>  // memcmp

// this
#include "globals.h"
#include "keyboard_helper.h"

// teletype
#include "teletype.h"
#include "teletype_io.h"

// meadowphysics engine + output binding + clock + grid (src/)
#include "meadowphysics_binding.h"
#include "meadowphysics_clock.h"
#include "meadowphysics_engine.h"
#include "meadowphysics_grid.h"

// libavr32
#include "events.h"
#include "font.h"
#include "monome.h"  // monomeLedBuffer, monome_is_vari
#include "music.h"   // SCALE_INT (diatonic mode intervals)
#include "region.h"
#include "timers.h"
#include "util.h"  // rnd, itoa

// asf
#include "conf_usb_host.h"  // needed in order to include "usb_protocol_hid.h"
#include "usb_protocol_hid.h"

// views (keyboard-selected, replacing Ansible's front-panel views)
#define MP_VIEW_POSITIONS 0
#define MP_VIEW_CLOCK 1
#define MP_VIEW_CONFIG 2

// tempo nudge per key press (ms of edge interval)
#define MP_TEMPO_STEP 4

// scales: 7 diatonic modes (from libavr32 SCALE_INT) + chromatic
#define MP_SCALE_COUNT 8
static const char* const mp_scale_name[MP_SCALE_COUNT] = {
    "IONIAN", "DORIAN",  "PHRYG",   "LYDIAN",
    "MIXOLY", "AEOLIAN", "LOCRIAN", "CHROMA"
};

static mp_engine_t mp_eng;
static mp_clock_t mp_clk;
static mp_grid_state_t mp_grid;
static softTimer_t mpClockTimer = { .next = NULL, .prev = NULL };

static bool initialized = false;  // engine/clock constructed once per session
static bool active = false;  // MP view is front-most (drives keyboard + grid)
static bool mp_running = false;  // engine playing: clock ticking, owns CV/TR
static bool writing = false;     // inside our own output write (ownership gate)
static bool timer_enabled = false;
static bool dirty = true;  // screen needs redraw
static uint8_t view = MP_VIEW_POSITIONS;

// RNG adapter for the MP_RULE_RND rule (engine takes an injected source).
static uint32_t mp_rnd(void* ctx) {
    (void)ctx;
    return rnd();
}

// Timer fires in ISR context; defer the actual step to the event loop
// (handler_AppCustom, data == 1), mirroring the metro timer.
static void mpClockTimer_callback(void* o) {
    (void)o;
    event_t e = { .type = kEventAppCustom, .data = 1 };
    event_post(&e);
}

static void run_clock(uint8_t phase) {
    writing = true;
    mp_engine_clock(&mp_eng, phase);
    writing = false;
    dirty = true;
}

// Recompute the engine's pitch table from the selected scale. Rows map to
// scale degrees over an octave; scale 0-6 are the diatonic modes (SCALE_INT),
// 7 is chromatic. Called on mode enter and whenever the scale changes.
static void mp_apply_scale(void) {
    uint8_t iv[8];
    uint8_t s = mp_eng.cfg.scale;
    iv[0] = 0;
    if (s < 7)
        for (uint8_t i = 0; i < 7; i++) iv[i + 1] = SCALE_INT[s][i];
    else  // chromatic
        for (uint8_t i = 1; i < 8; i++) iv[i] = 1;
    mp_engine_calc_scale(&mp_eng, iv);
}

// Load the current scene's MP config into the engine, sanitize it, arm the
// counters, and rebuild the pitch table.
static void mp_load_from_scene(void) {
    mp_eng.cfg = scene_state.mp;
    // A stale/old-layout flash scene can hold out-of-range values that would
    // index out of bounds; fall back to defaults if so.
    if (!mp_engine_config_valid(&mp_eng.cfg))
        mp_engine_set_defaults(&mp_eng.cfg);
    if (mp_eng.cfg.scale >= MP_SCALE_COUNT) mp_eng.cfg.scale = 0;
    mp_engine_reset(&mp_eng);
    mp_apply_scale();
}

// Construct the engine/clock/grid once per session and load the scene config.
static void mp_init_once(void) {
    if (initialized) return;
    mp_engine_init(&mp_eng, mp_binding_output(), &mp_rnd, NULL);
    mp_clock_init(&mp_clk);
    mp_grid_state_init(&mp_grid);
    mp_load_from_scene();
    initialized = true;
}

// Enter the MP view (front-most). Does NOT start the engine -- MP runs
// independently of whether you're looking at it (see meadowphysics_toggle_run).
void set_meadowphysics_mode(void) {
    mp_init_once();
    // If the scene's MP config changed while we were away (e.g. a scene was
    // loaded), reload it; otherwise leave a running sequence undisturbed.
    if (memcmp(&mp_eng.cfg, &scene_state.mp, sizeof(mp_config_t)) != 0)
        mp_load_from_scene();
    active = true;
    dirty = true;
}

// Leave the MP view. The engine keeps running in the background (MP owns the
// outputs until explicitly stopped); we only relinquish the keyboard/grid.
void meadowphysics_mode_exit(void) {
    // Persist the working config back to the scene so a later scene save
    // captures it.
    scene_state.mp = mp_eng.cfg;
    active = false;
}

// Play/pause the engine (Space in the MP view, or alt-P from anywhere).
// Running owns the CV/TR outputs; stopping releases them back to scripts.
void meadowphysics_toggle_run(void) {
    mp_init_once();
    if (mp_running) {
        mp_running = false;
        if (timer_enabled) {
            timer_remove(&mpClockTimer);
            timer_enabled = false;
        }
        // Release ownership: gates low. mp_running is now false, so these
        // writes pass the suppression gate; CV is left at its last value.
        for (uint8_t i = 0; i < 4; i++) tele_tr(i, 0);
    }
    else {
        mp_running = true;
        if (!timer_enabled) {
            timer_add(&mpClockTimer, mp_clk.period, &mpClockTimer_callback,
                      NULL);
            timer_enabled = true;
        }
    }
    dirty = true;
}

void meadowphysics_clock_tick(void) {
    if (!mp_running) return;
    uint8_t phase;
    if (mp_clock_internal_fire(&mp_clk, &phase)) run_clock(phase);
}

bool meadowphysics_external_clock(uint8_t level) {
    if (!mp_running || !mp_clk.external) return false;
    uint8_t phase;
    if (mp_clock_external_edge(&mp_clk, level, &phase)) run_clock(phase);
    return true;
}

bool meadowphysics_suppresses_output(void) {
    return mp_running && !writing;
}

// MP drives the monome grid when it's the front view OR while it's playing --
// so a running sequence keeps animating the grid as a visual aid even while
// you're editing in another mode. (Grid input then edits MP; pause MP to hand
// the grid back to the current mode.)
bool meadowphysics_owns_grid(void) {
    return active || mp_running;
}

void meadowphysics_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!meadowphysics_owns_grid()) return;
    mp_grid_process_key(&mp_eng, &mp_grid, x, y, z);
    dirty = true;
}

void meadowphysics_grid_render(void) {
    mp_grid_refresh(&mp_eng, &mp_grid, monomeLedBuffer, monome_is_vari());
}

void meadowphysics_op_reset(int16_t channel) {
    if (!initialized) return;
    if (channel <= 0)
        mp_engine_reset(&mp_eng);
    else if (channel <= MP_ROWS)
        mp_engine_reset_row(&mp_eng, channel - 1);
    dirty = true;
}

void meadowphysics_op_stop(int16_t channel) {
    if (!initialized) return;
    if (channel <= 0)
        mp_engine_stop(&mp_eng);
    else if (channel <= MP_ROWS)
        mp_engine_stop_row(&mp_eng, channel - 1);
    dirty = true;
}

static void set_period(uint16_t period_ms) {
    mp_clock_set_period(&mp_clk, period_ms);
    if (timer_enabled) mpClockTimer.ticks = mp_clk.period;
    dirty = true;
}

void process_meadowphysics_keys(uint8_t key, uint8_t mod_key,
                                bool is_held_key) {
    if (is_held_key) return;

    if (match_no_mod(mod_key, key, HID_1)) {
        view = MP_VIEW_POSITIONS;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_2)) {
        view = MP_VIEW_CLOCK;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_3)) {
        view = MP_VIEW_CONFIG;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_SPACEBAR)) {
        meadowphysics_toggle_run();  // play / pause
    }
    else if (match_no_mod(mod_key, key, HID_R)) {
        mp_engine_reset(&mp_eng);  // reset counters (independent of run state)
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_V)) {
        mp_eng.cfg.voice_mode = (mp_eng.cfg.voice_mode + 1) & 0x3;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_X)) {
        mp_clock_set_external(&mp_clk, !mp_clk.external);
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_UNDERSCORE)) {  // '-' : slower
        set_period(mp_clk.period + MP_TEMPO_STEP);
    }
    else if (match_no_mod(mod_key, key, HID_PLUS)) {  // '=' : faster
        set_period(mp_clk.period > MP_TEMPO_STEP ? mp_clk.period - MP_TEMPO_STEP
                                                 : MP_CLOCK_PERIOD_MIN);
    }
    else if (match_no_mod(mod_key, key,
                          HID_OPEN_BRACKET)) {  // '[' : prev scale
        mp_eng.cfg.scale =
            (mp_eng.cfg.scale + MP_SCALE_COUNT - 1) % MP_SCALE_COUNT;
        mp_apply_scale();
        dirty = true;
    }
    else if (match_no_mod(mod_key, key,
                          HID_CLOSE_BRACKET)) {  // ']' : next scale
        mp_eng.cfg.scale = (mp_eng.cfg.scale + 1) % MP_SCALE_COUNT;
        mp_apply_scale();
        dirty = true;
    }
}

// Brightness levels for the OLED (label / value / title).
#define MP_S_LABEL 5
#define MP_S_VALUE 12
#define MP_S_TITLE 15
#define MP_S_DIM 3

static const char* const mp_voice_name[4] = { "1V", "2V", "4V", "8T" };
static const char* const mp_rule_name[8] = { "NONE", "INC", "DEC",  "MAX",
                                             "MIN",  "RND", "POLE", "STOP" };
static const char* const mp_target_name[4] = { "-", "COUNT", "SPEED", "BOTH" };

// write a decimal number at (line, x)
static void mp_num(uint8_t ln, uint8_t x, int val, uint8_t fg) {
    char s[8];
    itoa(val, s, 10);
    font_string_region_clip(&line[ln], s, x, 0, fg, 0);
}

// OLED status view. A persistent header (L0-L3) plus a view-specific detail
// panel (L4-L7) selected by the keyboard 1/2/3 views. Net-new (Ansible has no
// display); complements the grid, which shows the counters visually.
uint8_t screen_refresh_meadowphysics(void) {
    if (!dirty) return 0;
    dirty = false;

    static const char* const grid_sub[3] = { "POS", "SPD", "RUL" };
    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    // --- header (all views) ---
    font_string_region_clip(&line[0], "MEADOWPHYSICS", 0, 0, MP_S_TITLE, 0);
    // view tabs, active one bright
    font_string_region_clip(&line[0], "P", 104, 0,
                            view == MP_VIEW_POSITIONS ? MP_S_TITLE : MP_S_DIM,
                            0);
    font_string_region_clip(&line[0], "C", 113, 0,
                            view == MP_VIEW_CLOCK ? MP_S_TITLE : MP_S_DIM, 0);
    font_string_region_clip(&line[0], "F", 122, 0,
                            view == MP_VIEW_CONFIG ? MP_S_TITLE : MP_S_DIM, 0);

    font_string_region_clip(&line[1], "VOICE", 0, 0, MP_S_LABEL, 0);
    font_string_region_clip(&line[1], mp_voice_name[mp_eng.cfg.voice_mode], 42,
                            0, MP_S_VALUE, 0);
    font_string_region_clip(&line[1], mp_running ? "RUN" : "STOP", 96, 0,
                            MP_S_VALUE, 0);

    font_string_region_clip(&line[2], "CLOCK", 0, 0, MP_S_LABEL, 0);
    font_string_region_clip(&line[2], mp_clk.external ? "EXT" : "INT", 42, 0,
                            MP_S_VALUE, 0);
    mp_num(2, 78, mp_clk.period, MP_S_VALUE);
    font_string_region_clip(&line[2], "MS", 108, 0, MP_S_LABEL, 0);

    font_string_region_clip(&line[3], "GRID", 0, 0, MP_S_LABEL, 0);
    font_string_region_clip(&line[3], grid_sub[mp_grid.edit_mode], 42, 0,
                            MP_S_VALUE, 0);
    font_string_region_clip(&line[3], "SCL", 78, 0, MP_S_LABEL, 0);
    mp_num(3, 108, mp_eng.cfg.scale, MP_S_VALUE);

    // --- detail panel (L4-L7), per keyboard-selected view ---
    if (view == MP_VIEW_CLOCK) {
        font_string_region_clip(&line[4], "CLOCK", 0, 0, MP_S_TITLE, 0);
        font_string_region_clip(&line[5], "SOURCE", 0, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[5],
                                mp_clk.external ? "EXT TR1" : "INTERNAL", 48, 0,
                                MP_S_VALUE, 0);
        font_string_region_clip(&line[6], "PERIOD", 0, 0, MP_S_LABEL, 0);
        mp_num(6, 48, mp_clk.period, MP_S_VALUE);
        font_string_region_clip(&line[6], "MS", 78, 0, MP_S_LABEL, 0);
        // one step = two clock edges, so steps/min = 30000 / period
        font_string_region_clip(&line[7], "STEP/M", 0, 0, MP_S_LABEL, 0);
        mp_num(7, 48, 30000 / mp_clk.period, MP_S_VALUE);
    }
    else if (view == MP_VIEW_CONFIG) {
        font_string_region_clip(&line[4], "CONFIG", 0, 0, MP_S_TITLE, 0);
        font_string_region_clip(&line[5], "VOICE", 0, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[5], mp_voice_name[mp_eng.cfg.voice_mode],
                                48, 0, MP_S_VALUE, 0);
        font_string_region_clip(&line[6], "SCALE", 0, 0, MP_S_LABEL, 0);
        font_string_region_clip(
            &line[6],
            mp_scale_name[mp_eng.cfg.scale < MP_SCALE_COUNT ? mp_eng.cfg.scale
                                                            : 0],
            48, 0, MP_S_VALUE, 0);
        font_string_region_clip(&line[7], "V:VOICE [ ]:SCALE", 0, 0, MP_S_DIM,
                                0);
    }
    else {  // MP_VIEW_POSITIONS: detail for the selected row
        uint8_t er = mp_grid.edit_row;
        font_string_region_clip(&line[4], "ROW", 0, 0, MP_S_LABEL, 0);
        mp_num(4, 30, er, MP_S_TITLE);
        font_string_region_clip(&line[5], "CNT", 0, 0, MP_S_LABEL, 0);
        mp_num(5, 30, mp_eng.cfg.count[er], MP_S_VALUE);
        font_string_region_clip(&line[5], "RNG", 66, 0, MP_S_LABEL, 0);
        mp_num(5, 96, mp_eng.cfg.min[er], MP_S_VALUE);
        font_string_region_clip(&line[5], "-", 108, 0, MP_S_LABEL, 0);
        mp_num(5, 114, mp_eng.cfg.max[er], MP_S_VALUE);
        font_string_region_clip(&line[6], "SPD", 0, 0, MP_S_LABEL, 0);
        mp_num(6, 30, mp_eng.cfg.speed[er], MP_S_VALUE);
        font_string_region_clip(&line[6], "RULE", 66, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[6],
                                mp_rule_name[mp_eng.cfg.rules[er] & 7], 102, 0,
                                MP_S_VALUE, 0);
        font_string_region_clip(&line[7], "DST R", 0, 0, MP_S_LABEL, 0);
        mp_num(7, 36, mp_eng.cfg.rule_dests[er], MP_S_VALUE);
        font_string_region_clip(
            &line[7], mp_target_name[mp_eng.cfg.rule_dest_targets[er] & 3], 66,
            0, MP_S_VALUE, 0);
    }

    return 0b11111111;
}
