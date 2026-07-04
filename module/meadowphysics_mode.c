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

// kria's i2c follower module (shared, global follower table)
#include "kria_i2c.h"
#include "kria_i2c_oled.h"  // shared MIDI-follower OLED editor

// libavr32
#include "events.h"
#include "flash.h"  // scale-bank persistence + MP_SCALE_SLOTS
#include "font.h"
#include "monome.h"  // monomeLedBuffer, monome_is_vari
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

// 16-slot editable scale bank (MP_SCALE_SLOTS, flash.h): slots 0-6 default to
// the diatonic modes, 7-15 to chromatic; all persisted and editable (the grid
// editor is Phase 2). Slots 0-7 have names; 8-15 show as USER.
#define MP_SCALE_NAMED 8
static const char* const mp_scale_name[MP_SCALE_NAMED] = {
    "IONIAN", "DORIAN",  "PHRYG",   "LYDIAN",
    "MIXOLY", "AEOLIAN", "LOCRIAN", "CHROMA"
};
static uint8_t mp_scale_bank[MP_SCALE_SLOTS][8];  // RAM mirror of f.scale_bank

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
static bool mp_bank_dirty =
    false;  // scale bank edited, not yet flushed to flash
static uint8_t view = MP_VIEW_POSITIONS;
static bool mp_i2c_view = false;  // grid shows the shared i2c view (keyboard 4)

// Flush the shared i2c follower bank to flash if it was edited.
static void mp_flush_i2c(void) {
    if (kria_i2c_take_dirty()) {
        kria_i2c_fstate_t t[KRIA_I2C_FOLLOWERS];
        kria_i2c_save(t);
        flash_update_kria_i2c(t);
    }
}

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

// Recompute the engine's pitch table from the selected scale-bank slot (rows
// map to scale degrees). Called on mode enter and whenever the scale changes.
static void mp_apply_scale(void) {
    mp_engine_calc_scale(&mp_eng, mp_scale_bank[mp_eng.cfg.scale]);
}

// Persist the scale bank to flash if edited. Batched (called on leaving the
// Config view / MP mode) rather than per keystroke, to spare flash wear.
static void mp_flush_bank(void) {
    if (!mp_bank_dirty) return;
    flash_update_scale_bank(mp_scale_bank);
    mp_bank_dirty = false;
}

// Load the current scene's MP config into the engine, sanitize it, arm the
// counters, and rebuild the pitch table.
static void mp_load_from_scene(void) {
    mp_eng.cfg = scene_state.mp;
    // A stale/old-layout flash scene can hold out-of-range values that would
    // index out of bounds; fall back to defaults if so.
    if (!mp_engine_config_valid(&mp_eng.cfg))
        mp_engine_set_defaults(&mp_eng.cfg);
    if (mp_eng.cfg.scale >= MP_SCALE_SLOTS) mp_eng.cfg.scale = 0;
    mp_engine_reset(&mp_eng);
    mp_apply_scale();
}

// MP output vtable with i2c follower fan-out. Mirrors meadowphysics_binding but
// also drives the shared Kria follower table (configure followers in Kria's i2c
// view; MP shares them). Additive to the CV/TR jacks.
static void mp_out_tr(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    tele_tr(ch, on);
    kria_i2c_tr(ch, on);
}
static void mp_out_cv(void* c, uint8_t ch, int16_t note) {
    (void)c;
    tele_cv(ch, mp_note_to_cv(note), 0);
    kria_i2c_set_voice(ch, note, 0);
    kria_i2c_cv(ch, mp_note_to_cv(note));
}
static void mp_out_cv_gate(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    tele_cv(ch, on ? MP_CV_FULL : 0, 0);
    // 8T CV-as-gate: physical CV jack stays 0-3, but drive follower gates 4-7 so
    // all 8 of 8T's gates are distinct at MIDI followers (cv_gate is 8T-only).
    kria_i2c_tr(ch + 4, on);
}
static const mp_output_t MP_OUT = {
    .tr = mp_out_tr, .cv = mp_out_cv, .cv_gate = mp_out_cv_gate, .ctx = NULL
};

// Construct the engine/clock/grid once per session and load the scene config.
static void mp_init_once(void) {
    if (initialized) return;
    mp_engine_init(&mp_eng, &MP_OUT, &mp_rnd, NULL);
    mp_clock_init(&mp_clk);
    mp_grid_state_init(&mp_grid);
    flash_get_scale_bank(mp_scale_bank);  // load bank before apply_scale
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
    mp_flush_bank();  // save any scale edits
    mp_flush_i2c();   // persist follower-bank edits
    kria_i2c_oled_exit();  // don't leave the MIDI editor open across mode exit
    mp_i2c_view = false;
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

// How many output channels (CV and TR, 0-indexed) MP claims for the current
// voice mode: 1V uses 1, 2V uses 2, 4V/8T use all 4. The rest are free.
static uint8_t mp_owned_channels(void) {
    switch (mp_eng.cfg.voice_mode) {
        case MP_1V: return 1;
        case MP_2V: return 2;
        default: return 4;  // 4V, 8T
    }
}

// True if a script write to output channel `ch` must be suppressed: MP is
// playing, this isn't MP's own write, and `ch` is one MP uses in this voice
// mode. Channels MP doesn't use stay free for scripts.
bool meadowphysics_suppresses_output(uint8_t ch) {
    return mp_running && !writing && ch < mp_owned_channels();
}

// MP drives the monome grid when it's the front view OR while it's playing --
// so a running sequence keeps animating the grid as a visual aid even while
// you're editing in another mode. (Grid input then edits MP; pause MP to hand
// the grid back to the current mode.)
bool meadowphysics_owns_grid(void) {
    return active || mp_running;
}

// The scale editor takes the grid only while you're actively viewing the Config
// view; when MP merely plays in the background the grid shows the sequence.
static bool mp_scale_editor_active(void) {
    return active && view == MP_VIEW_CONFIG;
}

void meadowphysics_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!meadowphysics_owns_grid()) return;
    if (active && mp_i2c_view) {
        kria_i2c_view_key(x, y, z);  // shared i2c follower view
        if (z) {
            int8_t req = kria_i2c_view_take_oled_req();
            if (req >= 0) kria_i2c_oled_enter((uint8_t)req);
        }
        dirty = true;
        return;
    }
    if (mp_scale_editor_active()) {
        if (mp_grid_scale_key(&mp_eng, mp_scale_bank, x, y, z))
            mp_bank_dirty = true;  // flushed on leaving the Config view / mode
        mp_apply_scale();          // slot or interval change updates live scale
    }
    else { mp_grid_process_key(&mp_eng, &mp_grid, x, y, z); }
    dirty = true;
}

void meadowphysics_grid_render(void) {
    if (active && mp_i2c_view) {
        kria_i2c_view_render(monomeLedBuffer, monome_is_vari());
        return;
    }
    if (mp_scale_editor_active())
        mp_grid_scale_refresh(&mp_eng, mp_scale_bank, monomeLedBuffer,
                              monome_is_vari());
    else
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

// MP.RUN x -- start (x != 0) or stop (x == 0) the engine, from scripts.
void meadowphysics_op_run(int16_t on) {
    if (on && !mp_running)
        meadowphysics_toggle_run();
    else if (!on && mp_running)
        meadowphysics_toggle_run();
}

static void set_period(uint16_t period_ms) {
    mp_clock_set_period(&mp_clk, period_ms);
    if (timer_enabled) mpClockTimer.ticks = mp_clk.period;
    dirty = true;
}

void process_meadowphysics_keys(uint8_t key, uint8_t mod_key,
                                bool is_held_key) {
    if (is_held_key) return;

    if (kria_i2c_oled_active()) {  // MIDI-follower editor has the keyboard
        if (kria_i2c_oled_key(key, mod_key, is_held_key)) {
            if (!kria_i2c_oled_active()) mp_flush_i2c();  // exited via <enter>
            dirty = true;
            return;
        }
        // not an editor key: leave the editor and process normally below.
        kria_i2c_oled_exit();
        mp_flush_i2c();
        dirty = true;
    }

    if (match_no_mod(mod_key, key, HID_1)) {
        view = MP_VIEW_POSITIONS;
        mp_flush_bank();  // leaving the Config view: save scale edits
        if (mp_i2c_view) mp_flush_i2c();
        mp_i2c_view = false;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_2)) {
        view = MP_VIEW_CLOCK;
        mp_flush_bank();
        if (mp_i2c_view) mp_flush_i2c();
        mp_i2c_view = false;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_3)) {
        view = MP_VIEW_CONFIG;
        if (mp_i2c_view) mp_flush_i2c();
        mp_i2c_view = false;
        dirty = true;
    }
    else if (match_no_mod(mod_key, key, HID_4)) {  // shared i2c follower view
        mp_i2c_view = true;
        kria_i2c_view_enter();
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
        // release gates on channels the new (narrower) voice mode no longer
        // uses, so scripts get clean TR channels
        if (mp_running)
            for (uint8_t i = mp_owned_channels(); i < 4; i++) tele_tr(i, 0);
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
            (mp_eng.cfg.scale + MP_SCALE_SLOTS - 1) % MP_SCALE_SLOTS;
        mp_apply_scale();
        dirty = true;
    }
    else if (match_no_mod(mod_key, key,
                          HID_CLOSE_BRACKET)) {  // ']' : next scale
        mp_eng.cfg.scale = (mp_eng.cfg.scale + 1) % MP_SCALE_SLOTS;
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

    if (kria_i2c_oled_active()) {  // MIDI-follower editor owns the screen
        kria_i2c_oled_render();
        return 0b11111111;
    }

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
        mp_num(6, 48, mp_eng.cfg.scale, MP_S_VALUE);  // slot number
        font_string_region_clip(&line[6],
                                mp_eng.cfg.scale < MP_SCALE_NAMED
                                    ? mp_scale_name[mp_eng.cfg.scale]
                                    : "USER",
                                72, 0, MP_S_VALUE, 0);
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
