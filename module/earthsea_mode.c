#include "earthsea_mode.h"

#include <string.h>

// this
#include "globals.h"
#include "keyboard_helper.h"
#include "mode_persist.h"  // shared save confirmation + flush

// teletype
#include "teletype.h"
#include "teletype_io.h"

// earthsea engine + binding + grid (src/)
#include "es_binding.h"
#include "es_engine.h"
#include "es_grid.h"
#include "helpers.h"        // note_to_cv (ES.CV, shared ET mapping)
#include "kria_i2c.h"       // shared follower bank + ii view
#include "kria_i2c_oled.h"  // MIDI-follower OLED editor

// libavr32
#include "events.h"
#include "flash.h"  // flash_get/update_es, scale bank
#include "font.h"
#include "init_teletype.h"  // get_ticks
#include "monome.h"         // monomeLedBuffer, monome_is_vari
#include "region.h"
#include "timers.h"
#include "util.h"  // itoa

// asf
#include "conf_usb_host.h"
#include "usb_protocol_hid.h"

// ---- instances / state ----

static es_engine_t eng;
static es_grid_state_t egrid;

static softTimer_t esPlayTimer = { .next = NULL, .prev = NULL };
static softTimer_t esNoteTimer[ES_NUM_VOICES];  // fixed-edge note-off
static softTimer_t esBlinkTimer = { .next = NULL, .prev = NULL };
static softTimer_t esPosTimer = { .next = NULL, .prev = NULL };  // UI tick
static uint8_t em_idx[ES_NUM_VOICES] = { 0, 1, 2, 3 };

static uint8_t es_scale_bank[MP_SCALE_SLOTS][8];  // shared w/ Kria + MP

static bool initialized = false;
static bool active = false;     // Earthsea view front-most
static bool writing = false;    // inside our own output write
static bool dirty = true;       // OLED
static bool cfg_dirty = false;  // bank edited, not yet flushed to flash

// Absolute deadline for the next play event (drift-free re-arm; see
// EARTHSEA_PORT_PLAN.md §7).
static uint32_t play_deadline = 0;

#define EM_VIEW_ES 0
#define EM_VIEW_I2C 1
static uint8_t em_view = EM_VIEW_ES;

static bool es_engaged(void) {
    return active || eng.rt.mode == es_playing;
}

// ---- output vtable (binding + fixed-edge note-off timer) ----

static void em_note_cb(void* o) {
    uint8_t v = *(uint8_t*)o;
    timer_remove(&esNoteTimer[v]);
    event_t e = { .type = kEventAppCustom,
                  .data = (int32_t)(ES_APPEVT_NOTEOFF_BASE + v) };
    event_post(&e);
}

static int16_t em_last_semi[ES_NUM_VOICES];  // last pitch per voice (ES.CV)

static void em_note_on(void* c, uint8_t voice, int16_t semitones,
                       uint16_t duration) {
    (void)c;
    em_last_semi[voice] = semitones;
    es_binding_note_on(voice, semitones, duration);
    timer_remove(&esNoteTimer[voice]);
    if (duration) {
        uint32_t d = duration;
        if (d < 1) d = 1;
        timer_add(&esNoteTimer[voice], d, &em_note_cb, &em_idx[voice]);
    }
}

static void em_note_off(void* c, uint8_t voice) {
    (void)c;
    timer_remove(&esNoteTimer[voice]);
    es_binding_note_off(voice);
}

static const es_output_t EM_OUT = { .note_on = em_note_on,
                                    .note_off = em_note_off,
                                    .ctx = NULL };

// ---- ISR-context timer callbacks: post events / set flags only ----

static void em_play_cb(void* o) {
    (void)o;
    timer_remove(&esPlayTimer);
    event_t e = { .type = kEventAppCustom, .data = ES_APPEVT_PLAY };
    event_post(&e);
}

static void em_blink_cb(void* o) {
    (void)o;
    egrid.blinker = !egrid.blinker;
    if (eng.rt.mode == es_recording && es_engaged()) {
        scene_state.grid.grid_dirty = 1;
        dirty = true;
    }
}

static void em_pos_cb(void* o) {
    (void)o;
    // keep the playback position bar moving (internal clock only)
    if (es_engaged() && eng.rt.mode == es_playing && !eng.rt.clock_external)
        scene_state.grid.grid_dirty = 1;
    if (mode_confirm_tick()) dirty = true;  // erase the SAVED banner
}

// ---- play-timer management (event-loop context) ----

// (Re)arm the play timer `interval` ticks after the previous deadline (or
// `now` when starting fresh). interval == 0 stops the chain.
static void em_arm_play(uint32_t interval, uint32_t now, bool fresh) {
    timer_remove(&esPlayTimer);
    if (!interval || eng.rt.mode != es_playing) return;
    if (fresh)
        play_deadline = now + interval;
    else {
        play_deadline += interval;
        // fell badly behind (long stall): resync instead of firing a burst
        if ((int32_t)(play_deadline - now) < 1) play_deadline = now + 1;
    }
    uint32_t delay = play_deadline - now;
    if (delay < 1) delay = 1;
    timer_add(&esPlayTimer, delay, &em_play_cb, NULL);
}

// After any engine call that may start/stop playback: sync the timer chain.
// `iv` is the engine's returned next-event interval (0 = none).
static void em_sync_transport(uint32_t iv, uint32_t now) {
    if (iv)
        em_arm_play(iv, now, true);
    else if (eng.rt.mode != es_playing)
        timer_remove(&esPlayTimer);
}

// ---- lifecycle / persistence ----

static void em_load_flash(void) {
    flash_get_es(&eng.cfg);
    if (!es_engine_config_valid(&eng.cfg)) es_engine_set_defaults(&eng.cfg);
}

// Persist the Earthsea bank (+ shared scale bank) and i2c follower bank if
// dirty. The single save path -- used by mode exit, the S key, and a scene
// save (via mode_persist_flush_all_dirty). Returns true if anything was
// written.
bool earthsea_flush_if_dirty(void) {
    bool wrote = false;
    if (cfg_dirty) {
        flash_update_es(&eng.cfg);
        flash_update_scale_bank(es_scale_bank);  // shared bank, small
        cfg_dirty = false;
        wrote = true;
    }
    if (mode_flush_i2c_if_dirty()) wrote = true;
    return wrote;
}

static void em_init_once(void) {
    if (initialized) return;
    flash_get_scale_bank(es_scale_bank);
    es_engine_init(&eng, &EM_OUT);
    es_grid_state_init(&egrid);
    egrid.scale_bank = es_scale_bank;
    em_load_flash();
    timer_add(&esBlinkTimer, 288, &em_blink_cb, NULL);
    timer_add(&esPosTimer, 50, &em_pos_cb, NULL);
    initialized = true;
}

void set_earthsea_mode(void) {
    em_init_once();
    active = true;
    dirty = true;
}

void earthsea_mode_exit(void) {
    earthsea_flush_if_dirty();  // bank + scale + i2c follower bank
    kria_i2c_oled_exit();       // don't leave the MIDI editor open across exit
    em_view = EM_VIEW_ES;
    active = false;
    // nothing playing in the background: silence live/drone notes and
    // release the jacks back to scripts
    if (eng.rt.mode != es_playing) {
        writing = true;
        es_engine_kill_all_notes(&eng);
        writing = false;
        if (eng.rt.mode == es_recording || eng.rt.mode == es_armed)
            es_engine_stop_recording(&eng, get_ticks());
    }
}

// ---- event-loop services ----

void es_service_play(void) {
    uint32_t now = get_ticks();
    writing = true;
    uint32_t iv = es_engine_play_advance(&eng, now);
    writing = false;
    if (iv)
        em_arm_play(iv, now, false);  // chain from the previous deadline
    else
        timer_remove(&esPlayTimer);
    dirty = true;
}

void es_service_note_off(uint8_t voice) {
    if (voice >= ES_NUM_VOICES) return;
    writing = true;
    es_engine_note_off_voice(&eng, voice);
    writing = false;
    if (es_engaged()) scene_state.grid.grid_dirty = 1;
}

bool es_external_clock(uint8_t level) {
    if (!es_engaged() || !eng.rt.clock_external) return false;
    if (level) {
        writing = true;
        es_engine_clock_step(&eng, get_ticks());
        writing = false;
        scene_state.grid.grid_dirty = 1;
        dirty = true;
    }
    return true;
}

bool es_play_trigger(uint8_t level) {
    if (!es_engaged()) return false;
    if (level && eng.rt.mode != es_armed && eng.rt.mode != es_recording) {
        uint32_t now = get_ticks();
        writing = true;
        uint32_t iv = es_engine_start_playback(&eng, 0, now);
        writing = false;
        em_sync_transport(iv, now);
        scene_state.grid.grid_dirty = 1;
        dirty = true;
    }
    return true;
}

// ---- ownership ----

bool es_suppresses_output(uint8_t ch) {
    if (writing || ch >= ES_NUM_VOICES || !es_engaged()) return false;
    uint8_t mask = eng.cfg.voices | eng.cfg.p[eng.cfg.p_select].voices;
    return (mask >> ch) & 1;
}

bool es_owns_grid(void) {
    return es_engaged();
}

// ---- grid surface ----

void es_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!es_owns_grid()) return;
    if (active && em_view == EM_VIEW_I2C) { mode_i2c_view_grid_key(x, y, z); }
    else {
        uint32_t now = get_ticks();
        writing = true;
        uint32_t iv = es_grid_process_key(&eng, &egrid, x, y, z, now);
        writing = false;
        em_sync_transport(iv, now);
        if (z) cfg_dirty = true;
    }
    dirty = true;
}

void es_grid_render(void) {
    if (active && em_view == EM_VIEW_I2C)
        kria_i2c_view_render(monomeLedBuffer, monome_is_vari());
    else
        es_grid_refresh(&eng, &egrid, monomeLedBuffer, monome_is_vari(),
                        get_ticks());
}

// ---- keyboard ----

static void em_start_stop(void) {
    uint32_t now = get_ticks();
    writing = true;
    uint32_t iv;
    if (eng.rt.mode == es_playing) {
        es_engine_stop_playback(&eng);
        iv = 0;
    }
    else { iv = es_engine_start_playback(&eng, 0, now); }
    writing = false;
    em_sync_transport(iv, now);
}

static void em_select_pattern(int16_t p) {
    if (p < 0 || p >= ES_NUM_PATTERNS) return;
    uint32_t now = get_ticks();
    writing = true;
    es_engine_set_pattern(&eng, (uint8_t)p);
    uint32_t iv = es_engine_start_playback(&eng, 0, now);
    writing = false;
    em_sync_transport(iv, now);
}

static void em_toggle_external(void) {
    // Ansible handler_ESTrNormal: switching clock source mid-play kills
    // pattern notes; internal resume steps immediately.
    writing = true;
    es_engine_kill_pattern_notes(&eng);
    writing = false;
    eng.rt.clock_external = !eng.rt.clock_external;
    if (eng.rt.mode == es_playing) {
        if (eng.rt.clock_external) { timer_remove(&esPlayTimer); }
        else {
            // resume: step immediately and start a fresh timer chain
            uint32_t now = get_ticks();
            writing = true;
            uint32_t iv = es_engine_play_advance(&eng, now);
            writing = false;
            em_sync_transport(iv, now);
        }
    }
}

void process_earthsea_keys(uint8_t key, uint8_t mod_key, bool is_held_key) {
    if (is_held_key) return;

    // MIDI-follower editor: consumes the key (return) or exits + falls through.
    if (mode_i2c_oled_handle_key(key, mod_key, is_held_key, &dirty)) return;

    if (match_no_mod(mod_key, key, HID_SPACEBAR)) { em_start_stop(); }
    else if (match_no_mod(mod_key, key, HID_A)) {  // arm / disarm
        uint32_t now = get_ticks();
        writing = true;
        if (eng.rt.mode == es_armed || eng.rt.mode == es_recording)
            es_engine_stop_recording(&eng, now);
        else
            es_engine_arm(&eng);
        writing = false;
        if (eng.rt.mode != es_playing) timer_remove(&esPlayTimer);
    }
    else if (match_no_mod(mod_key, key, HID_X)) { em_toggle_external(); }
    else if (match_no_mod(mod_key, key, HID_OPEN_BRACKET)) {
        if (eng.cfg.p_select) em_select_pattern(eng.cfg.p_select - 1);
    }
    else if (match_no_mod(mod_key, key, HID_CLOSE_BRACKET)) {
        em_select_pattern(eng.cfg.p_select + 1);
    }
    else if (match_no_mod(mod_key, key, HID_S)) {  // explicit save
        earthsea_flush_if_dirty();
        mode_confirm_show("SAVED");
    }
    else if (match_no_mod(mod_key, key, HID_1)) {  // earthsea surface
        em_view = EM_VIEW_ES;
        scene_state.grid.grid_dirty = 1;
    }
    else if (match_no_mod(mod_key, key, HID_4)) {  // i2c follower view
        em_view = EM_VIEW_I2C;
        kria_i2c_view_enter();
        scene_state.grid.grid_dirty = 1;
    }
    else { return; }

    scene_state.grid.grid_dirty = 1;
    dirty = true;
}

// ---- native ops (ES.* retargeted from external-Ansible i2c to the engine) --
// All ensure the engine is constructed so ops work even before entering the
// mode. Semantics mirror Ansible's ii_es handlers.

void es_op_run(int16_t on) {
    em_init_once();
    uint32_t now = get_ticks();
    writing = true;
    uint32_t iv = 0;
    if (on) { iv = es_engine_start_playback(&eng, 0, now); }
    else {
        es_engine_stop_playback(&eng);
        es_engine_kill_all_notes(&eng);
    }
    writing = false;
    em_sync_transport(iv, now);
    dirty = true;
}

void es_op_pattern(int16_t p) {
    em_init_once();
    if (p < 0 || p >= ES_NUM_PATTERNS) return;
    es_engine_set_pattern(&eng, (uint8_t)p);
    cfg_dirty = true;
    dirty = true;
}

void es_op_clock(int16_t d) {
    (void)d;  // Ansible's ES_CLOCK ignores its argument too
    em_init_once();
    writing = true;
    es_engine_clock_step(&eng, get_ticks());
    writing = false;
    if (es_engaged()) scene_state.grid.grid_dirty = 1;
    dirty = true;
}

void es_op_reset(int16_t pos) {
    em_init_once();
    if (pos < 0) pos = 0;
    if (pos > 15) pos = 15;
    uint32_t now = get_ticks();
    writing = true;
    uint32_t iv = es_engine_start_playback(&eng, (uint8_t)pos, now);
    writing = false;
    em_sync_transport(iv, now);
    dirty = true;
}

void es_op_stop(void) {
    em_init_once();
    writing = true;
    es_engine_stop_playback(&eng);
    writing = false;
    timer_remove(&esPlayTimer);
    dirty = true;
}

void es_op_trans(int16_t d) {
    em_init_once();
    uint32_t now = get_ticks();
    writing = true;
    es_engine_transpose(&eng, d);
    uint32_t iv = es_engine_start_playback(&eng, 0, now);  // Ansible restarts
    writing = false;
    em_sync_transport(iv, now);
    cfg_dirty = true;
    dirty = true;
}

void es_op_magic(int16_t d) {
    em_init_once();
    writing = true;
    switch (d) {
        case 1: es_engine_half_speed(&eng); break;
        case 2: es_engine_double_speed(&eng); break;
        case 3: es_engine_set_linearize(&eng, 1); break;
        case 4: es_engine_set_linearize(&eng, 0); break;
        case 5: es_engine_set_direction(&eng, 0); break;
        case 6: es_engine_set_direction(&eng, 1); break;
        default: break;
    }
    writing = false;
    cfg_dirty = true;
    dirty = true;
}

void es_op_mode(int16_t d) {
    em_init_once();
    writing = true;
    if (d < 0 || d >= 16)
        es_engine_set_edge(&eng, ES_EDGE_PATTERN, 0);
    else if (d == 0)
        es_engine_set_edge(&eng, ES_EDGE_DRONE, 0);
    else
        es_engine_set_edge(&eng, ES_EDGE_FIXED, (uint16_t)((d + 1) << 4));
    writing = false;
    cfg_dirty = true;
    dirty = true;
}

int16_t es_op_cv(int16_t voice) {
    em_init_once();
    if (voice < 0 || voice >= ES_NUM_VOICES) return 0;
    return note_to_cv(em_last_semi[voice]);
}

// ---- OLED ----

#define EM_S_LABEL 5
#define EM_S_VALUE 12
#define EM_S_TITLE 15


uint8_t screen_refresh_earthsea(void) {
    if (!dirty) return 0;
    dirty = false;

    if (mode_i2c_oled_render_active()) return 0b11111111;

    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    static const char* const mode_name[] = { "STOP", "ARM", "REC", "PLAY" };
    const es_pattern_t* p = &eng.cfg.p[eng.cfg.p_select];

    const char* cmsg;
    const char* title = mode_confirm_active(&cmsg) ? cmsg : "EARTHSEA";
    font_string_region_clip(&line[0], title, 0, 0, EM_S_TITLE, 0);
    font_string_region_clip(&line[0], em_view == EM_VIEW_I2C ? "I2C" : "", 66,
                            0, EM_S_VALUE, 0);
    font_string_region_clip(&line[0], mode_name[eng.rt.mode & 3], 100, 0,
                            EM_S_VALUE, 0);

    font_string_region_clip(&line[1], "PATT", 0, 0, EM_S_LABEL, 0);
    mode_draw_num(1, 42, eng.cfg.p_select, EM_S_VALUE);
    mode_draw_num(1, 66, p->length, EM_S_LABEL);
    font_string_region_clip(&line[1], p->loop ? "LOOP" : "", 96, 0, EM_S_VALUE,
                            0);

    font_string_region_clip(&line[2], "EDGE", 0, 0, EM_S_LABEL, 0);
    if (p->edge == ES_EDGE_PATTERN)
        font_string_region_clip(&line[2], "PATT", 42, 0, EM_S_VALUE, 0);
    else if (p->edge == ES_EDGE_DRONE)
        font_string_region_clip(&line[2], "DRONE", 42, 0, EM_S_VALUE, 0);
    else {
        font_string_region_clip(&line[2], "FIXED", 42, 0, EM_S_VALUE, 0);
        mode_draw_num(2, 84, p->edge_time, EM_S_VALUE);
    }

    font_string_region_clip(&line[3], "CLOCK", 0, 0, EM_S_LABEL, 0);
    font_string_region_clip(&line[3], eng.rt.clock_external ? "EXT" : "INT", 42,
                            0, EM_S_VALUE, 0);
    font_string_region_clip(&line[3], eng.cfg.arp ? "ARP" : "", 84, 0,
                            EM_S_VALUE, 0);

    font_string_region_clip(
        &line[7], "SPC:PLAY A:ARM X:EXT [ ]:PATT S:SAVE 4:I2C", 0, 0, 3, 0);

    return 0b11111111;
}
