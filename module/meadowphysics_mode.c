#include "meadowphysics_mode.h"

#include <string.h>  // memset (preset glyph buffer)

// this
#include "globals.h"
#include "keyboard_helper.h"
#include "mode_persist.h"  // shared save confirmation

// teletype
#include "script.h"  // REGULAR_SCRIPT_COUNT (scripts 1-8 for MP_SCRIPT mode)
#include "teletype.h"
#include "teletype_io.h"

// meadowphysics engine + output binding + clock + grid (src/)
#include "helpers.h"  // note_to_cv (shared ET semitone mapping)
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
static grid_clock_t mp_clk;
static mp_grid_state_t mp_grid;
static softTimer_t mpClockTimer = { .next = NULL, .prev = NULL };
static softTimer_t mpMetroOffTimer = { .next = NULL,
                                       .prev = NULL };  // metro step off-edge
static softTimer_t mpUiTimer = { .next = NULL,
                                 .prev = NULL };  // banner self-clear tick

// Floor (ms) for the metro-clocked step's scheduled off-edge, so a very fast M
// still yields a usable gate.
#define MP_METRO_GATE_MIN 8

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

// MP is stored in a global 8-slot preset bank (f.mp_slots), decoupled from
// scenes -- ansible-style. The working config lives in mp_eng.cfg; each slot
// also carries a drawable 8x8 glyph (glyph[row] = column bitmask). See flash.h.
static uint8_t mp_cur_slot = 0;  // slot the working config last loaded/saved
static uint8_t mp_sel_slot = 0;  // slot highlighted in the preset browser
static uint8_t mp_working_glyph[8];  // editable glyph for the working config
static bool mp_preset_view = false;  // grid shows the 8-slot preset browser (5)

// Double-tap the already-selected slot in the preset browser to load it: a
// second tap within MP_DBLTAP_TICKS UI ticks (100 ms each) of the first.
#define MP_DBLTAP_TICKS 4
static uint8_t mp_tap_slot = 0xFF;  // slot tapped first (double-tap detection)
static uint8_t mp_tap_ticks = 0;    // remaining ticks in the double-tap window

// RNG adapter for the MP_RULE_RND rule (engine takes an injected source).
static uint32_t mp_rnd(void* ctx) {
    (void)ctx;
    return rnd();
}

// Timer fires in ISR context; defer the actual step to the event loop
// (handler_AppCustom, data == 1), mirroring the metro timer.
static void mpClockTimer_callback(void* o) {
    (void)o;
    event_t e = { .type = kEventAppCustom, .data = MP_APPEVT_CLOCK };
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

// Always-on UI tick (MP has no other periodic timer while stopped): clears the
// transient "SAVED" banner after its delay.
static void mp_ui_cb(void* o) {
    (void)o;
    if (mp_tap_ticks) mp_tap_ticks--;  // expire the preset double-tap window
    if (mode_confirm_tick()) dirty = true;
}

// Load a preset slot (config + glyph) from the global bank into the engine,
// sanitize it, arm the counters, and rebuild the pitch table.
static void mp_load_slot(uint8_t slot) {
    if (slot >= MP_SLOTS) slot = 0;
    flash_get_mp_slot(slot, &mp_eng.cfg, mp_working_glyph);
    // A stale/old-layout flash slot can hold out-of-range values that would
    // index out of bounds; fall back to defaults if so. An unseeded slot's
    // glyph is likewise garbage (no validity check of its own), so blank it --
    // matching ansible's default_mp, which zeroes glyphs on a fresh flash.
    if (!mp_engine_config_valid(&mp_eng.cfg)) {
        mp_engine_set_defaults(&mp_eng.cfg);
        memset(mp_working_glyph, 0, 8);
    }
    if (mp_eng.cfg.scale >= MP_SCALE_SLOTS) mp_eng.cfg.scale = 0;
    mp_engine_reset(&mp_eng);
    mp_apply_scale();
    mp_cur_slot = slot;
    mp_sel_slot = slot;
}

// Read a slot's glyph into `out` for preview/editing. A slot whose config fails
// validation was never seeded (in-place upgrade left garbage there), so return
// a blank glyph rather than random pixels -- same fallback as mp_load_slot.
static void mp_read_slot_glyph(uint8_t slot, uint8_t out[8]) {
    mp_config_t tmp;
    flash_get_mp_slot(slot, &tmp, out);
    if (!mp_engine_config_valid(&tmp)) memset(out, 0, 8);
}

// Save the working config + glyph into a preset slot and remember it as current
// (reloaded on next boot).
static void mp_save_slot(uint8_t slot) {
    if (slot >= MP_SLOTS) return;
    flash_update_mp_slot(slot, &mp_eng.cfg, mp_working_glyph);
    flash_update_mp_current(slot);
    mp_cur_slot = slot;
}

// Load a slot and surface a "LOAD n" banner. Shared by the L key and the preset
// browser's double-tap gesture.
static void mp_load_and_confirm(uint8_t slot) {
    mp_load_slot(slot);
    flash_update_mp_current(slot);
    char b[8] = "LOAD ";
    itoa(slot, b + 5, 10);
    mode_confirm_show(b);
}

// MP output vtable with i2c follower fan-out. Mirrors meadowphysics_binding but
// also drives the shared Kria follower table (configure followers in Kria's i2c
// view; MP shares them). Additive to the CV/TR jacks.
static void mp_out_tr(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    // MP_SCRIPT: each row (ch 0-7) fires the matching Teletype script on its
    // rising edge, fully replacing the CV/TR jacks. The script itself is what
    // drives outputs -- so MP claims no channel (see mp_owned_channels) and the
    // off-edge is a no-op (scripts are momentary).
    if (mp_eng.cfg.voice_mode == MP_SCRIPT) {
        if (on && ch < REGULAR_SCRIPT_COUNT) run_script(&scene_state, ch);
        return;
    }
    tele_tr(ch, on);
    kria_i2c_tr(ch, on);
}
static void mp_out_cv(void* c, uint8_t ch, int16_t note) {
    (void)c;
    int16_t cv = note_to_cv(note);
    tele_cv(ch, cv, 0);
    kria_i2c_set_voice(ch, note, 0);
    kria_i2c_cv(ch, cv);
}
static void mp_out_cv_gate(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    tele_cv(ch, on ? MP_CV_FULL : 0, 0);
    // 8T CV-as-gate: physical CV jack stays 0-3, but drive follower gates 4-7
    // so all 8 of 8T's gates are distinct at MIDI followers (cv_gate is
    // 8T-only).
    kria_i2c_tr(ch + 4, on);
}
static const mp_output_t MP_OUT = {
    .tr = mp_out_tr, .cv = mp_out_cv, .cv_gate = mp_out_cv_gate, .ctx = NULL
};

// Construct the engine/clock/grid once per session and load the scene config.
static void mp_init_once(void) {
    if (initialized) return;
    mp_engine_init(&mp_eng, &MP_OUT, &mp_rnd, NULL);
    grid_clock_init(&mp_clk, MP_CLOCK_PERIOD_MIN, MP_CLOCK_PERIOD_MAX,
                    MP_CLOCK_PERIOD_DEFAULT);
    mp_grid_state_init(&mp_grid);
    flash_get_scale_bank(mp_scale_bank);  // load bank before apply_scale
    // restore the last-used preset slot
    mp_load_slot(flash_get_mp_current());
    timer_add(&mpUiTimer, 100, &mp_ui_cb, NULL);  // banner self-clear
    initialized = true;
}

// Enter the MP view (front-most). Does NOT start the engine -- MP runs
// independently of whether you're looking at it (see meadowphysics_toggle_run).
void set_meadowphysics_mode(void) {
    mp_init_once();
    // MP is a global preset bank now, independent of the loaded scene, so
    // entering the view leaves the working config (and any running sequence)
    // untouched -- it only changes via L / S in the preset browser.
    active = true;
    dirty = true;
}

// Leave the MP view. The engine keeps running in the background (MP owns the
// outputs until explicitly stopped); we only relinquish the keyboard/grid.
void meadowphysics_mode_exit(void) {
    // Working config edits are volatile until saved to a slot (S) -- ansible
    // semantics -- so nothing config-side is persisted here.
    mp_flush_bank();            // save any scale edits
    mode_flush_i2c_if_dirty();  // persist follower-bank edits
    kria_i2c_oled_exit();  // don't leave the MIDI editor open across mode exit
    mp_i2c_view = false;
    mp_preset_view = false;
    active = false;
}

// Bring the internal soft timer into line with the run state and clock source:
// it drives the engine only while playing on the INTERNAL clock. The EXT (Tr)
// and METRO sources feed run_clock() from elsewhere, so the timer must be off
// under them (else two sources would advance the sequencer at once).
static void mp_sync_clock_timer(void) {
    bool want = mp_running && !mp_clk.external && !mp_clk.metro;
    if (want && !timer_enabled) {
        timer_add(&mpClockTimer, mp_clk.period, &mpClockTimer_callback, NULL);
        timer_enabled = true;
    }
    else if (!want && timer_enabled) {
        timer_remove(&mpClockTimer);
        timer_enabled = false;
    }
}

// Play/pause the engine (Space in the MP view, or the MP.RUN script op).
// Running owns the CV/TR outputs; stopping releases them back to scripts.
void meadowphysics_toggle_run(void) {
    mp_init_once();
    mp_running = !mp_running;
    mp_sync_clock_timer();
    if (!mp_running) {
        // Release ownership: gates low. mp_running is now false, so these
        // writes pass the suppression gate; CV is left at its last value.
        for (uint8_t i = 0; i < 4; i++) tele_tr(i, 0);
    }
    // repaint the grid too: on stop there is no further clock tick to clear the
    // last-lit frame, and the script (MP.RUN) path never touches the grid.
    scene_state.grid.grid_dirty = 1;
    dirty = true;
}

void meadowphysics_clock_tick(void) {
    if (!mp_running) return;
    uint8_t phase;
    if (grid_clock_internal_fire(&mp_clk, &phase)) run_clock(phase);
}

bool meadowphysics_external_clock(uint8_t level) {
    if (!mp_running || !mp_clk.external) return false;
    uint8_t phase;
    if (grid_clock_external_edge(&mp_clk, level, &phase)) run_clock(phase);
    return true;
}

bool meadowphysics_external_reset(uint8_t level) {
    if (!mp_running || !mp_clk.external) return false;
    if (level) mp_engine_reset(&mp_eng);
    return true;
}

// Fires in ISR context; defer the actual off-edge to the event loop.
static void mpMetroOff_callback(void* o) {
    (void)o;
    timer_remove(&mpMetroOffTimer);  // one-shot
    event_t e = { .type = kEventAppCustom, .data = MP_APPEVT_METRO_OFF };
    event_post(&e);
}

// Advance one full step: fire the on-edge now, then schedule the off-edge
// `gate_ms` later (a gate rather than an instantaneous edge, so CV/TR voice
// modes get a usable pulse). Used by the metro sync and the MP.CLK op.
static void mp_full_step(uint16_t gate_ms) {
    // Drain the previous step's pending off-edge before this on-edge so state[]
    // resets and the retrigger is re-detected as a fresh rising edge under fast
    // tempo. Without this, a shared cancellable off-timer could be removed
    // before it fired, leaving state[] high so mp_engine_clock suppresses the
    // note-on entirely (dropped note). Mirrors the internal grid metro's
    // alternating two-phase clock; no-op if the off already fired.
    run_clock(0);
    timer_remove(&mpMetroOffTimer);
    run_clock(1);
    if (gate_ms < MP_METRO_GATE_MIN) gate_ms = MP_METRO_GATE_MIN;
    timer_add(&mpMetroOffTimer, gate_ms, &mpMetroOff_callback, NULL);
}

void meadowphysics_metro_tick(void) {
    if (!mp_running || !mp_clk.metro) return;
    // One metro tick = one full step, gate = half the metro interval (a ~50%
    // duty, matching the internal clock's two-edges-per-step). M is clamped
    // positive by the M / M! ops, so m/2 is a safe gate length.
    mp_full_step((uint16_t)(scene_state.variables.m / 2));
}

void meadowphysics_metro_off(void) {
    if (mp_running) run_clock(0);
}

// How many output channels (CV and TR, 0-indexed) MP claims for the current
// voice mode: 1V uses 1, 2V uses 2, 4V/8T use all 4. The rest are free.
static uint8_t mp_owned_channels(void) {
    switch (mp_eng.cfg.voice_mode) {
        case MP_SCRIPT: return 0;  // fires scripts, drives no jack itself
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

// Preset browser: left column (x==0) rows 0-7 pick the S/L target slot; the
// right 8x8 block (x>=8) is the drawable glyph canvas for the working config.
static void mp_preset_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!z || y >= 8) return;  // act on press, 8 rows only
    if (x == 0) {
        if (mp_tap_ticks && mp_tap_slot == y) {  // double-tap: load + close
            mp_tap_ticks = 0;
            mp_load_and_confirm(y);
            // close the browser and hand the grid back to the running sequence
            // (the POSITIONS view, as if the user pressed 1)
            mp_preset_view = false;
            view = MP_VIEW_POSITIONS;
        }
        else {  // single tap: select it, preview its glyph, arm double-tap
            mp_sel_slot = y;
            mp_read_slot_glyph(y, mp_working_glyph);
            mp_tap_slot = y;
            mp_tap_ticks = MP_DBLTAP_TICKS;
        }
    }
    else if (x >= 8)
        mp_working_glyph[y] ^= 1 << (x - 8);
}

// Render the preset browser: slot column (selected brightest, current mid, rest
// dim) + the working glyph in the right 8x8.
static void mp_preset_grid_refresh(uint8_t* led, bool varibright) {
    memset(led, 0, MP_ROWS * 16);
    for (uint8_t s = 0; s < MP_SLOTS; s++)
        led[s * 16] = (s == mp_sel_slot)   ? 15
                      : (s == mp_cur_slot) ? (varibright ? 8 : 15)
                                           : (varibright ? 3 : 0);
    for (uint8_t r = 0; r < 8; r++)
        for (uint8_t c = 0; c < 8; c++)
            if (mp_working_glyph[r] & (1 << c))
                led[r * 16 + 8 + c] = varibright ? 12 : 15;
}

void meadowphysics_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!meadowphysics_owns_grid()) return;
    if (active && mp_i2c_view) {
        mode_i2c_view_grid_key(x, y, z);  // shared i2c follower view
        dirty = true;
        return;
    }
    if (active && mp_preset_view) {
        mp_preset_grid_key(x, y, z);
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
    if (active && mp_preset_view) {
        mp_preset_grid_refresh(monomeLedBuffer, monome_is_vari());
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
    grid_clock_set_period(&mp_clk, period_ms);
    if (timer_enabled) mpClockTimer.ticks = mp_clk.period;
    dirty = true;
}

// Cycle the clock source: INTERNAL -> EXT (Tr) -> METRO (Teletype M) -> ...
// Under EXT/METRO the internal soft timer is suppressed; mp_sync_clock_timer
// re-derives it from the new source.
static void mp_cycle_clock_source(void) {
    if (mp_clk.metro) {  // METRO -> INTERNAL
        mp_clk.metro = false;
    }
    else if (mp_clk.external) {  // EXT -> METRO
        grid_clock_set_external(&mp_clk, false);
        mp_clk.metro = true;
    }
    else {  // INTERNAL -> EXT
        grid_clock_set_external(&mp_clk, true);
    }
    mp_sync_clock_timer();
}

// ---- MP.* script ops (each constructs the engine/bank first) ----

int16_t meadowphysics_op_sync_get(void) {
    mp_init_once();
    if (mp_clk.metro) return 2;
    return mp_clk.external ? 1 : 0;
}

void meadowphysics_op_sync_set(int16_t src) {
    mp_init_once();
    grid_clock_set_external(&mp_clk, src == 1);
    mp_clk.metro = (src == 2);
    mp_sync_clock_timer();
    dirty = true;
}

void meadowphysics_op_clock(void) {
    mp_init_once();
    if (!mp_running) return;
    mp_full_step(mp_clk.period / 2);  // gate scaled to the configured tempo
}

int16_t meadowphysics_op_voice_get(void) {
    mp_init_once();
    return mp_eng.cfg.voice_mode;
}

void meadowphysics_op_voice_set(int16_t mode) {
    mp_init_once();
    if (mode < 0 || mode >= MP_VOICE_MODE_COUNT) return;
    mp_eng.cfg.voice_mode = (uint8_t)mode;
    // release gates on channels the new voice mode no longer uses (SCRIPT uses
    // none), so scripts get clean TR channels
    if (mp_running)
        for (uint8_t i = mp_owned_channels(); i < 4; i++) tele_tr(i, 0);
    dirty = true;
}

int16_t meadowphysics_op_period_get(void) {
    mp_init_once();
    return mp_clk.period;
}

void meadowphysics_op_period_set(int16_t ms) {
    mp_init_once();
    if (ms < 0) ms = 0;
    set_period((uint16_t)ms);  // clamps to [MIN, MAX] and retunes the timer
}

int16_t meadowphysics_op_scale_get(void) {
    mp_init_once();
    return mp_eng.cfg.scale;
}

void meadowphysics_op_scale_set(int16_t slot) {
    mp_init_once();
    if (slot < 0 || slot >= MP_SCALE_SLOTS) return;
    mp_eng.cfg.scale = (uint8_t)slot;
    mp_apply_scale();  // rebuild the live pitch table from the slot
    dirty = true;
}

int16_t meadowphysics_op_ladder_get(int16_t slot, int16_t degree) {
    mp_init_once();
    if (slot < 0 || slot >= MP_SCALE_SLOTS || degree < 0 || degree >= 8)
        return 0;
    // Return the cumulative note offset (sum of rungs 0..degree) -- the actual
    // semitone the engine plays for this degree -- not the raw per-rung delta.
    // Mirrors mp_engine_calc_scale. (The setter still writes a single delta.)
    int16_t note = 0;
    for (int16_t i = 0; i <= degree; i++) note += mp_scale_bank[slot][i];
    return note;
}

void meadowphysics_op_ladder_set(int16_t slot, int16_t degree, int16_t val) {
    mp_init_once();
    if (slot < 0 || slot >= MP_SCALE_SLOTS || degree < 0 || degree >= 8) return;
    if (val < 0) val = 0;
    if (val > 7) val = 7;  // match the on-grid scale editor's 0-7 range
    mp_scale_bank[slot][degree] = (uint8_t)val;
    mp_bank_dirty = true;  // flushed to flash on the next Config-view/mode exit
    if ((uint8_t)slot == mp_eng.cfg.scale)
        mp_apply_scale();  // live update if editing the active slot
    dirty = true;
}

int16_t meadowphysics_op_preset_get(void) {
    mp_init_once();
    return mp_cur_slot;
}

void meadowphysics_op_preset_set(int16_t slot) {
    mp_init_once();
    if (slot < 0 || slot >= MP_SLOTS) return;
    mp_load_slot((uint8_t)slot);  // load config + glyph, arm counters
    flash_update_mp_current((uint8_t)slot);  // reload this slot on next boot
    dirty = true;
}

void process_meadowphysics_keys(uint8_t key, uint8_t mod_key,
                                bool is_held_key) {
    if (is_held_key) return;

    // MIDI-follower editor: consumes the key (return) or exits + falls through.
    if (mode_i2c_oled_handle_key(key, mod_key, is_held_key, &dirty)) return;

    if (match_no_mod(mod_key, key, HID_1)) {
        view = MP_VIEW_POSITIONS;
        mp_flush_bank();  // leaving the Config view: save scale edits
        if (mp_i2c_view) mode_flush_i2c_if_dirty();
        mp_i2c_view = false;
        mp_preset_view = false;
    }
    else if (match_no_mod(mod_key, key, HID_2)) {
        view = MP_VIEW_CLOCK;
        mp_flush_bank();
        if (mp_i2c_view) mode_flush_i2c_if_dirty();
        mp_i2c_view = false;
        mp_preset_view = false;
    }
    else if (match_no_mod(mod_key, key, HID_3)) {
        view = MP_VIEW_CONFIG;
        if (mp_i2c_view) mode_flush_i2c_if_dirty();
        mp_i2c_view = false;
        mp_preset_view = false;
    }
    else if (match_no_mod(mod_key, key, HID_4)) {  // shared i2c follower view
        mp_i2c_view = true;
        mp_preset_view = false;
        kria_i2c_view_enter();
    }
    else if (match_no_mod(mod_key, key, HID_5)) {  // preset (slot) browser
        mp_preset_view = true;
        if (mp_i2c_view) mode_flush_i2c_if_dirty();
        mp_i2c_view = false;
        mp_sel_slot = mp_cur_slot;  // start on the current slot
    }
    else if (match_no_mod(mod_key, key, HID_SPACEBAR)) {
        meadowphysics_toggle_run();  // play / pause
    }
    else if (match_no_mod(mod_key, key, HID_R)) {
        mp_engine_reset(&mp_eng);  // reset counters (independent of run state)
    }
    else if (match_no_mod(mod_key, key, HID_S)) {  // save -> selected slot
        // Commit the working config + glyph to the highlighted preset slot
        // (mp_sel_slot; defaults to the current slot when the browser is
        // closed). MP is a global bank, decoupled from the Teletype scene.
        mp_save_slot(mp_sel_slot);
        mp_flush_bank();            // shared scale bank, if edited
        mode_flush_i2c_if_dirty();  // shared follower bank, if edited
        char b[8] = "SAVE ";
        itoa(mp_sel_slot, b + 5, 10);
        mode_confirm_show(b);
    }
    else if (match_no_mod(mod_key, key, HID_L)) {  // load <- selected slot
        mp_load_and_confirm(mp_sel_slot);
    }
    else if (match_no_mod(mod_key, key, HID_V)) {
        mp_eng.cfg.voice_mode =
            (mp_eng.cfg.voice_mode + 1) % MP_VOICE_MODE_COUNT;
        // release gates on channels the new voice mode no longer uses (SCRIPT
        // uses none), so scripts get clean TR channels
        if (mp_running)
            for (uint8_t i = mp_owned_channels(); i < 4; i++) tele_tr(i, 0);
    }
    else if (match_no_mod(mod_key, key, HID_X)) {
        mp_cycle_clock_source();  // INT -> EXT (Tr) -> METRO (M)
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
    }
    else if (match_no_mod(mod_key, key,
                          HID_CLOSE_BRACKET)) {  // ']' : next scale
        mp_eng.cfg.scale = (mp_eng.cfg.scale + 1) % MP_SCALE_SLOTS;
        mp_apply_scale();
    }
    else { return; }

    // every handled key repaints both the OLED and the grid
    scene_state.grid.grid_dirty = 1;
    dirty = true;
}

// Brightness levels for the OLED (label / value / title).
#define MP_S_LABEL 5
#define MP_S_VALUE 12
#define MP_S_TITLE 15
#define MP_S_DIM 3

static const char* const mp_voice_name[MP_VOICE_MODE_COUNT] = { "1V", "2V",
                                                                "4V", "8T",
                                                                "SCR" };
static const char* const mp_rule_name[8] = { "NONE", "INC", "DEC",  "MAX",
                                             "MIN",  "RND", "POLE", "STOP" };
static const char* const mp_target_name[4] = { "-", "COUNT", "SPEED", "BOTH" };

// Short clock-source label for the header (INT / EXT / M).
static const char* mp_clock_src_short(void) {
    if (mp_clk.metro) return "M";
    return mp_clk.external ? "EXT" : "INT";
}

// write a decimal number at (line, x)

// OLED status view. A persistent header (L0-L3) plus a view-specific detail
// panel (L4-L7) selected by the keyboard 1/2/3 views. Net-new (Ansible has no
// display); complements the grid, which shows the counters visually.
uint8_t screen_refresh_meadowphysics(void) {
    if (!dirty) return 0;
    dirty = false;

    if (mode_i2c_oled_render_active()) return 0b11111111;

    static const char* const grid_sub[3] = { "POS", "SPD", "RUL" };
    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    // --- header (all views) ---
    const char* cmsg;
    const char* title = mode_confirm_active(&cmsg) ? cmsg : "MEADOWPHYSICS";
    font_string_region_clip(&line[0], title, 0, 0, MP_S_TITLE, 0);
    // Current MP preset slot (global 8-slot bank, not the Teletype scene).
    font_string_region_clip(&line[0], "SL", 74, 0, MP_S_DIM, 0);
    char slotbuf[4];
    itoa(mp_cur_slot, slotbuf, 10);
    font_string_region_clip(&line[0], slotbuf, 90, 0, MP_S_DIM, 0);
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

    // metro-synced: the step interval is M, not the (unused) internal period
    uint16_t step_ms =
        mp_clk.metro ? (uint16_t)scene_state.variables.m : mp_clk.period;
    font_string_region_clip(&line[2], "CLOCK", 0, 0, MP_S_LABEL, 0);
    font_string_region_clip(&line[2], mp_clock_src_short(), 42, 0, MP_S_VALUE,
                            0);
    mode_draw_num(2, 78, step_ms, MP_S_VALUE);
    font_string_region_clip(&line[2], "MS", 108, 0, MP_S_LABEL, 0);

    font_string_region_clip(&line[3], "GRID", 0, 0, MP_S_LABEL, 0);
    font_string_region_clip(&line[3], grid_sub[mp_grid.edit_mode], 42, 0,
                            MP_S_VALUE, 0);
    font_string_region_clip(&line[3], "SCL", 78, 0, MP_S_LABEL, 0);
    mode_draw_num(3, 108, mp_eng.cfg.scale, MP_S_VALUE);

    // --- detail panel (L4-L7), per keyboard-selected view ---
    if (mp_preset_view) {
        font_string_region_clip(&line[4], "PRESET", 0, 0, MP_S_TITLE, 0);
        font_string_region_clip(&line[5], "SLOT", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(5, 48, mp_sel_slot, MP_S_VALUE);
        font_string_region_clip(&line[6], "CUR", 66, 0, MP_S_LABEL, 0);
        mode_draw_num(6, 96, mp_cur_slot, MP_S_VALUE);
        font_string_region_clip(&line[7], "S:SAVE L:LOAD GRID:GLYPH", 0, 0,
                                MP_S_DIM, 0);
    }
    else if (view == MP_VIEW_CLOCK) {
        font_string_region_clip(&line[4], "CLOCK", 0, 0, MP_S_TITLE, 0);
        const char* src = "INTERNAL";
        if (mp_clk.metro)
            src = "TT M";
        else if (mp_clk.external)
            src = "EXT TR1";
        // metro-synced: one full step per M tick (interval = M, steps/min =
        // 60000 / M). Internal: two edges per step -> 30000 / period.
        uint16_t period =
            mp_clk.metro ? (uint16_t)scene_state.variables.m : mp_clk.period;
        uint16_t per_step = mp_clk.metro ? 60000 : 30000;
        uint16_t spm = period ? (uint16_t)(per_step / period) : 0;
        font_string_region_clip(&line[5], "SOURCE", 0, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[5], src, 48, 0, MP_S_VALUE, 0);
        font_string_region_clip(&line[6], "PERIOD", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(6, 48, period, MP_S_VALUE);
        font_string_region_clip(&line[6], "MS", 78, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[7], "STEP/M", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(7, 48, spm, MP_S_VALUE);
    }
    else if (view == MP_VIEW_CONFIG) {
        font_string_region_clip(&line[4], "CONFIG", 0, 0, MP_S_TITLE, 0);
        font_string_region_clip(&line[5], "VOICE", 0, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[5], mp_voice_name[mp_eng.cfg.voice_mode],
                                48, 0, MP_S_VALUE, 0);
        font_string_region_clip(&line[6], "SCALE", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(6, 48, mp_eng.cfg.scale, MP_S_VALUE);  // slot number
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
        mode_draw_num(4, 30, er, MP_S_TITLE);
        font_string_region_clip(&line[5], "CNT", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(5, 30, mp_eng.cfg.count[er], MP_S_VALUE);
        font_string_region_clip(&line[5], "RNG", 66, 0, MP_S_LABEL, 0);
        mode_draw_num(5, 96, mp_eng.cfg.min[er], MP_S_VALUE);
        font_string_region_clip(&line[5], "-", 108, 0, MP_S_LABEL, 0);
        mode_draw_num(5, 114, mp_eng.cfg.max[er], MP_S_VALUE);
        font_string_region_clip(&line[6], "SPD", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(6, 30, mp_eng.cfg.speed[er], MP_S_VALUE);
        font_string_region_clip(&line[6], "RULE", 66, 0, MP_S_LABEL, 0);
        font_string_region_clip(&line[6],
                                mp_rule_name[mp_eng.cfg.rules[er] & 7], 102, 0,
                                MP_S_VALUE, 0);
        font_string_region_clip(&line[7], "DST R", 0, 0, MP_S_LABEL, 0);
        mode_draw_num(7, 36, mp_eng.cfg.rule_dests[er], MP_S_VALUE);
        font_string_region_clip(
            &line[7], mp_target_name[mp_eng.cfg.rule_dest_targets[er] & 3], 66,
            0, MP_S_VALUE, 0);
    }

    return 0b11111111;
}
