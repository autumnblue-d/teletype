#include "kria_mode.h"

#include <string.h>

// this
#include "globals.h"
#include "keyboard_helper.h"
#include "mode_persist.h"  // shared save confirmation + flush

// teletype
#include "teletype.h"
#include "teletype_io.h"

// kria engine + clock + grid (src/)
#include "grid_led.h"  // GRID_L0/1/2 ramp + grid_led_finalize
#include "helpers.h"   // note_to_cv (plain ET, for the i2c fan-out)
#include "kria_clock.h"
#include "kria_engine.h"
#include "kria_grid.h"
#include "kria_i2c.h"       // follower output
#include "kria_i2c_oled.h"  // MIDI-follower OLED editor
#include "meadowphysics_engine.h"  // MP-style cascade seq (DUR sub-tab)
#include "tuning.h"         // per-channel tuning table + editor helpers

// libavr32
#include "events.h"
#include "flash.h"  // flash_get/update_kria, scale bank, MP_SCALE_SLOTS
#include "font.h"
#include "init_teletype.h"  // get_ticks
#include "monome.h"         // monomeLedBuffer, monome_is_vari
#include "region.h"
#include "timers.h"
#include "util.h"  // rnd, itoa

// asf
#include "conf_usb_host.h"
#include "usb_protocol_hid.h"

#define KM_TEMPO_STEP 4

// ---- instances / state ----

static kria_engine_t eng;
static grid_clock_t clk;
static kria_grid_state_t kgrid;

// MP-style cascade sequencer (DUR page's second sub-tab). The engine is a
// separate struct from the Kria engine; its cfg is a working copy of the active
// pattern's mpseq (mp_config_t is embedded by value, and MP rules mutate it
// live). km_mp_pattern tracks which pattern that copy belongs to so we can write
// it back and reload on pattern changes. See KRIA_MPSEQ_PLAN.md.
static mp_engine_t km_mp;
static uint8_t km_mp_pattern = 0;
static void km_mp_writeback(void);   // defined in the output-vtable section
static void km_mp_load_active(void);

static softTimer_t kriaClockTimer = { .next = NULL, .prev = NULL };
static softTimer_t auxTimer[KRIA_NUM_TRACKS];     // note-off
static softTimer_t repeatTimer[KRIA_NUM_TRACKS];  // repeat retrigger
static softTimer_t blinkTimer[KRIA_NUM_TRACKS];   // grid trigger blink
static softTimer_t kriaBlinkTimer = { .next = NULL, .prev = NULL };  // alt/meta
static uint8_t km_idx[KRIA_NUM_TRACKS] = { 0, 1, 2, 3 };

static uint8_t kria_scale_bank[MP_SCALE_SLOTS]
                              [8];  // shared w/ MP (f.scale_bank)

static bool initialized = false;
static bool active = false;        // Kria view front-most
static bool kria_running = false;  // engine playing (timer on, owns outputs)
static bool writing = false;       // inside our own output write
static bool in_repeat = false;  // inside a repeat retrigger (gate scheduling)
static bool timer_enabled = false;
static bool dirty = true;
static bool cfg_dirty = false;  // song edited, not yet flushed to flash

// mPattern long-press: blink-timer ticks a slot has been held (100 ms each).
// At the threshold we post KR_APPEVT_PATTERN_COPY (~Ansible GRID_KEY_HOLD_TIME).
#define KR_PATTERN_HOLD_TICKS 4
static uint8_t hold_ticks = 0;

static uint64_t last_tick_time = 0;
static uint32_t clock_delta = KR_CLOCK_PERIOD_DEFAULT;

// Grid views (Ansible Key 1 / Key 2 equivalents), switched from the keyboard
// (1 = sequencer, 2 = time, 3 = config). Only active while the Kria view is
// front-most; a background-playing engine keeps showing the sequencer.
#define KM_VIEW_SEQ 0
#define KM_VIEW_TIME 1
#define KM_VIEW_CONFIG 2
#define KM_VIEW_I2C 3
#define KM_VIEW_TUNING 4  // Ansible advanced "tuning" grid editor
static uint8_t km_view = KM_VIEW_SEQ;
static uint8_t km_rough = 0;
static uint8_t km_fine = 0;

// ---- tuning-view state (Ansible ansible_grid.c view_tuning) ----
// Edits the global tuning_table[] RAM copy live; persisted only on the explicit
// save gestures below. Note preview drives the selected slot(s) to the module
// CV/TR outs so a tuner/scope can read them.
static uint8_t tun_track = 0;   // selected channel (0..3)
static uint8_t tun_octave = 0;  // selected octave bank (0..9)
static uint8_t tun_offset[4] = { 0, 0, 0, 0 };  // per-track note within octave
static bool tun_note_on[4] = { true, true, true, true };
static bool tun_mod = false;  // row4/col0 held: fine edits hit all slots
// Row-5 utility keys use short/long-press (Ansible grid_keytimer). Timed on
// release from get_ticks() rather than a hold timer, to keep flash writes in
// the grid event-loop context.
#define KM_TUNING_HOLD_MS 400
static uint8_t tun_hold_x = 0xff;  // row-5 utility key currently held
static uint32_t tun_hold_start = 0;

// Persist the Kria song bank (+ shared scale bank) and i2c follower bank if
// dirty. The single save path -- used by mode exit, the S key, and a scene
// save (via mode_persist_flush_all_dirty). Returns true if anything was
// written.
bool kria_flush_if_dirty(void) {
    bool wrote = false;
    // Capture the working MP-seq config into the active pattern so its (possibly
    // rule-evolved / edited) state is included in the flash write.
    km_mp_writeback();
    if (cfg_dirty) {
        flash_update_kria(&eng.cfg);
        flash_update_scale_bank(kria_scale_bank);
        cfg_dirty = false;
        wrote = true;
    }
    if (mode_flush_i2c_if_dirty()) wrote = true;
    return wrote;
}

static void km_set_period(uint16_t p);  // defined in the keyboard section
static void km_tuning_enter(void);       // defined in the tuning-view section
static void km_tuning_leave(void);

static int imax(int a, int b) {
    return a > b ? a : b;
}

static uint32_t km_rnd(void* ctx) {
    (void)ctx;
    return rnd();
}

// ---- output vtable (adds gate-timer scheduling on note-on) ----

static void km_schedule_gate(uint8_t ch);

static void km_tr(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    tele_tr(ch, on);
    kria_i2c_tr(ch, on);  // fan out to i2c followers (additive)
    if (on) km_schedule_gate(ch);
}
static void km_cv(void* c, uint8_t ch, int16_t sem) {
    (void)c;
    if (ch < KRIA_NUM_TRACKS) {
        // i2c followers stay on the plain ET map (matches Ansible).
        kria_i2c_set_voice(ch, sem, eng.rt.dur_unscaled[ch]);
        kria_i2c_cv(ch, note_to_cv(sem));
    }
    // module CV out is retuned per channel via the tuning table.
    tele_cv(ch, note_to_cv_ch(ch, sem), 1);
}
static void km_slew(void* c, uint8_t ch, uint16_t s) {
    (void)c;
    tele_cv_slew(ch, (int16_t)s);
    kria_i2c_slew(ch, s);
}
static const kria_output_t KM_OUT = {
    .tr = km_tr, .cv = km_cv, .cv_slew = km_slew, .ctx = NULL
};

// ---- MP-seq output vtable: each lane's rising edge fires a native script ----
// voice_mode is pinned to MP_SCRIPT, so the engine only calls tr() (never
// cv()/cv_gate()). Lane n -> script KRIA_SCRIPT_BASE + n (scripts 3-8); the
// unused lanes 6-7 are dropped here. Off-edges (on == 0) are momentary and
// ignored, exactly as the standalone MP mode's MP_SCRIPT binding.
static void km_mp_tr(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    if (on && ch < KRIA_SCRIPT_LANES) run_script(&scene_state, KRIA_SCRIPT_BASE + ch);
}
static void km_mp_cv(void* c, uint8_t ch, int16_t note) {
    (void)c;
    (void)ch;
    (void)note;
}
static void km_mp_cv_gate(void* c, uint8_t ch, uint8_t on) {
    (void)c;
    (void)ch;
    (void)on;
}
static const mp_output_t KM_MP_OUT = {
    .tr = km_mp_tr, .cv = km_mp_cv, .cv_gate = km_mp_cv_gate, .ctx = NULL
};

// Copy the working MP config back into the pattern it belongs to (so evolved
// rule state / edits are captured before a flush or a pattern reload).
static void km_mp_writeback(void) {
    if (km_mp_pattern < KRIA_NUM_PATTERNS)
        eng.cfg.p[km_mp_pattern].mpseq = km_mp.cfg;
}

// Load the active pattern's MP config into the working engine and re-arm it.
static void km_mp_load_active(void) {
    km_mp_pattern = eng.cfg.pattern;
    km_mp.cfg = eng.cfg.p[km_mp_pattern].mpseq;
    mp_engine_reset(&km_mp);
}

// ---- ISR-context timer callbacks: post events / clear flags only ----

static void km_clock_cb(void* o) {
    (void)o;
    event_t e = { .type = kEventAppCustom, .data = KR_APPEVT_CLOCK };
    event_post(&e);
}
static void km_aux_cb(void* o) {
    uint8_t t = *(uint8_t*)o;
    timer_remove(&auxTimer[t]);
    event_t e = { .type = kEventAppCustom,
                  .data = (int32_t)(KR_APPEVT_NOTEOFF_BASE + t) };
    event_post(&e);
}
static void km_rpt_cb(void* o) {
    uint8_t t = *(uint8_t*)o;
    timer_remove(&repeatTimer[t]);
    event_t e = { .type = kEventAppCustom,
                  .data = (int32_t)(KR_APPEVT_REPEAT_BASE + t) };
    event_post(&e);
}
static void km_blink_cb(void* o) {
    uint8_t t = *(uint8_t*)o;
    timer_remove(&blinkTimer[t]);
    kgrid.blinks[t] = 0;
    scene_state.grid.grid_dirty = 1;
}
static void km_altblink_cb(void* o) {
    (void)o;
    kgrid.alt_blink ^= 1;
    kgrid.meta_lock_blink ^= 1;
    if ((active || kria_running)) scene_state.grid.grid_dirty = 1;
    if (mode_confirm_tick()) dirty = true;  // erase the SAVED banner
    // mPattern long-press: once a slot has been held past the threshold, post
    // the copy event (the memcpy itself runs in the event loop, not here).
    if (kgrid.hold_pending) {
        if (++hold_ticks >= KR_PATTERN_HOLD_TICKS) {
            hold_ticks = 0;
            event_t ev = { .type = kEventAppCustom,
                           .data = KR_APPEVT_PATTERN_COPY };
            event_post(&ev);
        }
    }
    else
        hold_ticks = 0;
}

// Schedule the note-off (and, on the initial clock fire, the first repeat) for
// a track whose gate just went high. Real-tick scaling from the measured clock
// length (Ansible's dur/rpt + rptTicks). Runs in the event loop (not ISR).
// Minimum note-off delay (ms). Ansible's Kria gate is a short CV *trigger* — as
// little as ~2-5 ms at the default duration — which is inaudible as a MIDI note
// (the synth never articulates it, so notes appear dropped). Floor the gate so
// every MIDI note is long enough to sound; harmless as a CV/TR trigger.
#define KR_GATE_MIN_MS 30

static void km_schedule_gate(uint8_t ch) {
    kria_track_t* t = &eng.cfg.p[eng.cfg.pattern].t[ch];
    uint8_t rpt = eng.rt.rpt[ch];
    if (!rpt) rpt = 1;
    uint16_t dur = kria_clock_scale_duration(eng.rt.dur_unscaled[ch],
                                             clock_delta, t->tmul[KR_P_TR]);
    uint32_t off = dur / rpt;
    // Floor the gate to a usable MIDI note length (Ansible's short CV-trigger
    // gate is inaudible as a MIDI note).
    uint32_t gate_min;
    if (rpt > 1) {
        // Ratchet: make each repeat last until just before the next one — the
        // longest audible length that still ends before the next note-on (so it
        // doesn't overlap/race it). Fast/high-count ratchets are inherently
        // short; this is the best we can do without them running together.
        uint32_t spacing =
            kria_clock_repeat_ticks(clock_delta, t->tmul[KR_P_TR], rpt);
        gate_min = spacing > 4 ? spacing - 3 : spacing;
    }
    else {
        // Single note: an absolute usable minimum, but capped below the step so
        // the note still ends before the next one at fast tempo.
        gate_min = KR_GATE_MIN_MS;
        uint32_t cap = (clock_delta * 3) / 4;
        if (cap && gate_min > cap) gate_min = cap;
    }
    if (off < gate_min) off = gate_min;
    if (off < 1) off = 1;

    timer_remove(&auxTimer[ch]);
    timer_add(&auxTimer[ch], off, &km_aux_cb, &km_idx[ch]);

    kgrid.blinks[ch] = 1;
    timer_remove(&blinkTimer[ch]);
    timer_add(&blinkTimer[ch], (uint32_t)imax((int)off, 31), &km_blink_cb,
              &km_idx[ch]);

    if (!in_repeat && eng.rt.repeats[ch] > 0) {
        uint32_t rt =
            kria_clock_repeat_ticks(clock_delta, t->tmul[KR_P_TR], rpt);
        if (rt < 1) rt = 1;
        timer_remove(&repeatTimer[ch]);
        timer_add(&repeatTimer[ch], rt, &km_rpt_cb, &km_idx[ch]);
    }
}

static void run_clock(uint8_t phase) {
    if (phase) {
        uint64_t now = get_ticks();
        if (last_tick_time) clock_delta = (uint32_t)(now - last_tick_time);
        last_tick_time = now;
    }
    // Kria's own pos-reset (reset input / KR.RESET) re-arms the MP lanes too.
    // Capture it before the engine consumes the flag.
    bool mp_reset = phase && eng.rt.pos_reset;

    writing = true;
    kria_engine_clock(&eng, phase);
    writing = false;
    // Fire native scripts 3-8 for any script-lane trigger points that landed on
    // this clock. The engine only flags them (rt.script_fired); scene_state and
    // run_script live here in the shell (mirrors Meadowphysics' MP_SCRIPT).
    if (phase && eng.rt.script_fired) {
        uint8_t fired = eng.rt.script_fired;
        eng.rt.script_fired = 0;
        for (uint8_t lane = 0; lane < KRIA_SCRIPT_LANES; lane++)
            if (fired & (1u << lane))
                run_script(&scene_state, KRIA_SCRIPT_BASE + lane);
    }

    // MP-style cascade seq (DUR sub-tab). Keep the working config synced to the
    // active pattern -- meta/cue pattern changes happen inside the call above --
    // then advance. Firing goes straight to scripts via KM_MP_OUT.
    if (eng.cfg.pattern != km_mp_pattern) {
        km_mp_writeback();
        km_mp_load_active();
    }
    else if (mp_reset) {
        mp_engine_reset(&km_mp);
    }
    mp_engine_clock(&km_mp, phase);

    dirty = true;
}

// ---- lifecycle / persistence ----

static void km_apply_scale(void) {
    kria_engine_calc_scale(&eng,
                           kria_scale_bank[eng.cfg.p[eng.cfg.pattern].scale]);
}

static void km_load_flash(void) {
    flash_get_kria(&eng.cfg);
    if (!kria_engine_config_valid(&eng.cfg)) kria_engine_set_defaults(&eng.cfg);
    kria_engine_reset(&eng);
    km_apply_scale();
    km_mp_load_active();  // load the active pattern's MP-seq config + re-arm
}

static void km_init_once(void) {
    if (initialized) return;
    flash_get_scale_bank(kria_scale_bank);
    kria_engine_init(&eng, &KM_OUT, &km_rnd, NULL, kria_scale_bank);
    // MP-seq engine: bind outputs + RNG once; its cfg is loaded per-pattern by
    // km_load_flash / km_mp_load_active below.
    km_mp.out = KM_MP_OUT;
    km_mp.rnd = &km_rnd;
    km_mp.rnd_ctx = NULL;
    grid_clock_init(&clk, KR_CLOCK_PERIOD_MIN, KR_CLOCK_PERIOD_MAX,
                    KR_CLOCK_PERIOD_DEFAULT);
    kria_grid_state_init(&kgrid);
    kgrid.scale_bank = kria_scale_bank;
    km_load_flash();
    // (i2c follower bank is global and loaded at boot in main.c)
    grid_clock_set_period(&clk, eng.cfg.clock_period ? eng.cfg.clock_period
                                                     : KR_CLOCK_PERIOD_DEFAULT);
    timer_add(&kriaBlinkTimer, 100, &km_altblink_cb, NULL);
    initialized = true;
}

void set_kria_mode(void) {
    km_init_once();
    active = true;
    dirty = true;
}

void kria_mode_exit(void) {
    // Persist edits (song is a global bank, not per-scene). ~18 KB flash write
    // only when something changed; may briefly stall -- prefer saving stopped.
    kria_flush_if_dirty();  // song + scale + i2c follower bank
    kria_i2c_oled_exit();   // don't leave the MIDI editor open across mode exit
    if (km_view == KM_VIEW_TUNING) km_tuning_leave();  // drop preview gates
    km_view = KM_VIEW_SEQ;  // next entry starts on the sequencer
    kgrid.hold_pending = 0;  // drop any in-flight pattern long-press
    hold_ticks = 0;
    active = false;
}

void kria_toggle_run(void) {
    km_init_once();
    if (kria_running) {
        kria_running = false;
        if (timer_enabled) {
            timer_remove(&kriaClockTimer);
            timer_enabled = false;
        }
        for (uint8_t i = 0; i < KRIA_NUM_TRACKS; i++) {
            timer_remove(&auxTimer[i]);
            timer_remove(&repeatTimer[i]);
            tele_tr(i, 0);  // kria_running now false -> passes the gate
        }
    }
    else {
        kria_running = true;
        last_tick_time = 0;
        if (!timer_enabled) {
            timer_add(&kriaClockTimer, clk.period, &km_clock_cb, NULL);
            timer_enabled = true;
        }
    }
    // repaint the grid too: on stop there is no further clock tick to clear the
    // last-lit playhead, and the script (KR.RUN) path never touches the grid.
    scene_state.grid.grid_dirty = 1;
    dirty = true;
}

// ---- event-loop services ----

void kria_clock_tick(void) {
    if (!kria_running) return;
    uint8_t phase;
    if (grid_clock_internal_fire(&clk, &phase)) run_clock(phase);
}

void kria_service_note_off(uint8_t track) {
    if (track >= KRIA_NUM_TRACKS) return;
    writing = true;
    kria_engine_note_off(&eng, track);
    writing = false;
    dirty = true;
}

void kria_service_repeat(uint8_t track) {
    if (track >= KRIA_NUM_TRACKS) return;
    in_repeat = true;
    writing = true;
    kria_engine_repeat(&eng, track);
    writing = false;
    in_repeat = false;
    if (eng.rt.repeats[track] > 0) {
        kria_track_t* t = &eng.cfg.p[eng.cfg.pattern].t[track];
        uint8_t rpt = eng.rt.rpt[track];
        if (!rpt) rpt = 1;
        uint32_t rt =
            kria_clock_repeat_ticks(clock_delta, t->tmul[KR_P_TR], rpt);
        if (rt < 1) rt = 1;
        timer_remove(&repeatTimer[track]);
        timer_add(&repeatTimer[track], rt, &km_rpt_cb, &km_idx[track]);
    }
    dirty = true;
}

void kria_service_pattern_copy(void) {
    // cfg_dirty was already set by the initiating press (kria_grid_key).
    if (kria_grid_pattern_hold_fire(&eng, &kgrid)) {
        dirty = true;
        if (kria_owns_grid()) scene_state.grid.grid_dirty = 1;
    }
}

bool kria_external_clock(uint8_t level) {
    if (!kria_running || !clk.external) return false;
    uint8_t phase;
    if (grid_clock_external_edge(&clk, level, &phase)) run_clock(phase);
    return true;
}

// Reset edge from handler_Trigger for KR_EXT_RESET_INPUT (jack 2). Only active
// -- and only consumes the edge -- while the external clock is enabled; resets
// the sequencer (tracks + script lanes) on the rising edge.
bool kria_external_reset(uint8_t level) {
    if (!kria_running || !clk.external) return false;
    if (level) {
        kria_engine_reset(&eng);
        scene_state.grid.grid_dirty = 1;
        dirty = true;
    }
    return true;
}

// ---- ownership ----

bool kria_suppresses_output(uint8_t ch) {
    return kria_running && !writing && ch < KRIA_NUM_TRACKS &&
           !eng.rt.mutes[ch];
}

bool kria_owns_grid(void) {
    return active || kria_running;
}

// ---- Time / Config grid views (Ansible Key 1 / Key 2 equivalents) ----

#define KM_LD GRID_L0  // dim
#define KM_LB GRID_L2  // bright

static void km_view_finalize(uint8_t* led) {
    grid_led_finalize(led, monome_is_vari());
}

// tempo <-> rough/fine (Ansible: period = 20 + rough*16 + fine).
static void km_sync_rc_from_period(void) {
    int d = (int)clk.period - 20;
    if (d < 0) d = 0;
    km_rough = d / 16;
    if (km_rough > 15) km_rough = 15;
    km_fine = d % 16;
}
static void km_apply_rc(void) {
    km_set_period((uint16_t)(20 + km_rough * 16 + km_fine));
}

// Time view -- matches Ansible refresh_clock (grid_time_interval1.3): pulse
// indicator (row 0), rough (row 1) / fine (row 2) selected cells, DEC/INC
// keyset (row 4 x6-9), the note-division-sync box (cols 0-3 rows 4-7), the
// sync-mode block (x7-8 rows 6-7) and division-sync (x12 r5 / x12-15 r7).
static void km_time_render(void) {
    uint8_t* led = monomeLedBuffer;
    uint8_t i;
    memset(led, 0, 128);
    led[eng.rt.clock_count & 0x0f] = KM_LD;  // pulse indicator (row 0)
    if (clk.external) {
        memset(led + 16, 3, 16);  // ext: division-mult row (cosmetic)
    }
    else {
        led[16 + km_rough] = 12;  // rough (row 1)
        led[32 + km_fine] = 8;    // fine (row 2)
        led[64 + 6] = 7;          // DEC/INC keyset (row 4)
        led[64 + 7] = 3;
        led[64 + 8] = 3;
        led[64 + 9] = 7;
    }
    i = kgrid.note_div_sync ? 7 : 3;  // note-division-sync box (cols 0-3, r4-7)
    led[64 + 0] = i;
    led[80 + 0] = i;
    led[96 + 0] = i;
    led[112 + 0] = i;
    led[64 + 1] = i;
    led[64 + 2] = i;
    led[64 + 3] = i;
    led[80 + 3] = i;
    led[96 + 3] = i;
    led[112 + 3] = i;
    led[112 + 2] = i;
    led[112 + 1] = i;
    i = (eng.cfg.sync_mode & KR_SYNC_TIMEDIV) ? 7
                                              : 3;  // sync-mode (x7-8, r6-7)
    led[96 + 7] = i;
    led[96 + 8] = i;
    led[112 + 7] = i;
    led[112 + 8] = i;
    led[80 + 12] =
        (kgrid.div_sync == 1) ? 7 : 3;  // division-sync: track (x12 r5)
    i = (kgrid.div_sync == 2) ? 7 : 3;  // all (x12-15 r7)
    led[112 + 12] = i;
    led[112 + 13] = i;
    led[112 + 14] = i;
    led[112 + 15] = i;
    km_view_finalize(led);
}

static void km_time_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!z) return;
    if (!clk.external) {
        if (y == 1) {
            km_rough = x;
            km_apply_rc();
        }
        else if (y == 2) {
            km_fine = x;
            km_apply_rc();
        }
        else if (y == 4 && x >= 6 && x <= 9) {  // incremental time adjust
            int inc = (x == 6) ? -4 : (x == 7) ? -1 : (x == 8) ? 1 : 4;
            int p = (int)clk.period + inc;
            if (p < KR_CLOCK_PERIOD_MIN) p = KR_CLOCK_PERIOD_MIN;
            km_set_period((uint16_t)p);
            km_sync_rc_from_period();
        }
    }
    if (y >= 4 && x <= 3) kgrid.note_div_sync ^= 1;
    if (x >= 7 && x <= 8 && y >= 6) {
        eng.cfg.sync_mode ^= KR_SYNC_TIMEDIV;
        cfg_dirty = true;
    }
    if (x >= 12 && y == 5) kgrid.div_sync = (kgrid.div_sync == 1) ? 0 : 1;
    if (x >= 12 && y == 7) kgrid.div_sync = (kgrid.div_sync == 2) ? 0 : 2;
}

// Config view -- matches Ansible refresh_kria_config (grid_KR_config):
// brightness (row 0 x0-2), the note-sync box (cols 2-5 rows 2-5), loop-sync
// (x10 r3 = track, x10-13 r5 = all), note-tie (x8 r7), tuning (x14 r7),
// meta-reset (x15 r7).
static void km_config_render(void) {
    uint8_t* led = monomeLedBuffer;
    uint8_t i;
    memset(led, 0, 128);
    memset(led, 4, 3);                   // brightness options (row 0 x0-2)
    led[monome_is_vari() ? 2 : 0] = 12;  // current grid type (auto)
    i = kgrid.note_sync ? 7 : 3;         // note-sync box (cols 2-5, r2-5)
    led[32 + 2] = i;
    led[32 + 3] = i;
    led[32 + 4] = i;
    led[32 + 5] = i;
    led[48 + 2] = i;
    led[48 + 5] = i;
    led[64 + 2] = i;
    led[64 + 5] = i;
    led[80 + 2] = i;
    led[80 + 3] = i;
    led[80 + 4] = i;
    led[80 + 5] = i;
    led[48 + 10] = (kgrid.loop_sync == 1) ? 7 : 3;  // loop-sync: track (x10 r3)
    i = (kgrid.loop_sync == 2) ? 7 : 3;             // all (x10-13 r5)
    led[80 + 10] = i;
    led[80 + 11] = i;
    led[80 + 12] = i;
    led[80 + 13] = i;
    led[112 + 8] = eng.cfg.dur_tie_mode ? 8 : 4;     // note-tie (x8 r7)
    led[112 + 14] = 4;                               // tuning button (x14 r7)
    led[112 + 15] = eng.cfg.meta_reset_all ? 8 : 4;  // meta-reset (x15 r7)
    km_view_finalize(led);
}

static void km_config_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!z) return;
    if (x < 8 && y > 0 && y < 7)
        kgrid.note_sync ^= 1;
    else if (y == 0 && x < 3) {
        // grid brightness is auto-detected here; kept for layout parity
    }
    else if (y == 3)
        kgrid.loop_sync = (kgrid.loop_sync == 1) ? 0 : 1;
    else if (y == 5)
        kgrid.loop_sync = (kgrid.loop_sync == 2) ? 0 : 2;
    else if (y == 7 && x == 8) {
        eng.cfg.dur_tie_mode = !eng.cfg.dur_tie_mode;
        cfg_dirty = true;
    }
    else if (y == 7 && x == 15) {
        eng.cfg.meta_reset_all = !eng.cfg.meta_reset_all;
        cfg_dirty = true;
    }
    else if (y == 7 && x == 14) {  // enter the tuning editor (Ansible parity)
        km_view = KM_VIEW_TUNING;
        km_tuning_enter();
    }
}

// The i2c view (Ansible ii toggle + per-follower config pages) is shared with
// the MP shell; it lives in src/kria_i2c.c (kria_i2c_view_*).

// ---- Tuning view (Ansible refresh_grid_tuning / view_tuning) ----
// A faithful port of Ansible's CV tuning editor, hosted inside Kria mode.
// See https://monome.org/docs/ansible/advanced/#tuning.

static uint16_t tun_slot(uint8_t track) {
    return (uint16_t)tun_octave * 12 + tun_offset[track];
}

// Drive the module CV/TR outs so the edited slot(s) can be measured. writing=1
// bypasses Kria's own output suppression (harmless if the engine is stopped,
// which is the normal case in this config view).
static void km_tuning_preview(void) {
    writing = true;
    for (uint8_t t = 0; t < 4; t++) {
        if (tun_mod || t == tun_track)
            tele_cv(t, (int16_t)tuning_table[t][tun_slot(t)], 0);
        tele_tr(t, tun_note_on[t] ? 1 : 0);
    }
    writing = false;
}

static void km_tuning_enter(void) {
    tun_octave = 0;
    tun_track = 0;
    tun_mod = false;
    tun_hold_x = 0xff;
    for (uint8_t t = 0; t < 4; t++) {
        tun_note_on[t] = true;
        tun_offset[t] = 0;
    }
    km_tuning_preview();
}

static void km_tuning_leave(void) {
    writing = true;
    for (uint8_t t = 0; t < 4; t++) tele_tr(t, 0);  // drop preview gates
    writing = false;
}

static void km_tuning_render(void) {
    uint8_t* led = monomeLedBuffer;
    memset(led, 0, 128);

    // rows 0-3: per-track note-on toggle (col 0) + 12 note slots (cols 2-13)
    for (uint8_t i = 0; i < 4; i++) {
        led[i * 16] = tun_note_on[i] ? GRID_L1 : GRID_L0;
        for (uint8_t c = 0; c < 12; c++) led[i * 16 + 2 + c] = GRID_L0;
        led[i * 16 + 2 + tun_offset[i]] += 4;
        if (i == tun_track) led[i * 16 + 2 + tun_offset[i]] += 4;
    }

    // row 4: mod (fine edits apply to all slots while held)
    led[64] = tun_mod ? GRID_L1 : GRID_L0;

    // row 5: octave banks (0-9) + reload(11) / fit-offset(13) / fit-lin(14) /
    // save(15) utility keys
    for (uint8_t c = 0; c < 10; c++) led[80 + c] = GRID_L0;
    led[80 + tun_octave] = GRID_L1;
    led[80 + 11] = GRID_L1;
    led[80 + 13] = GRID_L1;
    led[80 + 14] = GRID_L1;
    led[80 + 15] = GRID_L1;

    // row 6: coarse DAC value of the selected slot, as a filled bar
    uint16_t v = tuning_table[tun_track][tun_slot(tun_track)];
    uint8_t dac_step = v >> 6;  // 14-bit -> 0..255
    memset(led + 96, 3, dac_step / 16);
    led[96 + dac_step / 16] = dac_step % 16;

    // row 7: fine +/- steps around the selected slot
    for (uint8_t i = 0; i < 8; i++) {
        if ((int32_t)TUNING_DAC_MAX - v > (1 << i))
            led[112 + 8 + i] = 2 * i + 1;
        if (v > (1 << i)) led[112 + 7 - i] = 2 * i + 1;
    }

    km_view_finalize(led);
}

// Commit or preview the row-5 utility keys. long_press mirrors Ansible's
// grid_keytimer (destructive/persisting), short press its immediate handler.
static void km_tuning_util(uint8_t x, bool long_press) {
    if (x == 11) {
        if (long_press)
            tuning_default();               // factory reset (equal temperament)
        else
            flash_get_tuning(tuning_table);  // reload last saved (panic)
    }
    else if (x == 13) {
        tuning_fit(0);  // fixed offset per channel
        if (long_press) flash_update_tuning(tuning_table);
    }
    else if (x == 14) {
        tuning_fit(1);  // linear interpolation between octave waypoints
        if (long_press) flash_update_tuning(tuning_table);
    }
    else if (x == 15) {
        flash_update_tuning(tuning_table);  // save as-is
        mode_confirm_show("SAVED");
    }
    km_tuning_preview();
}

static void km_tuning_key(uint8_t x, uint8_t y, uint8_t z) {
    if (z) {
        if (y == 4 && x == 0) { tun_mod = true; }
        else if (y <= 3) {
            if (x == 0) { tun_note_on[y] = !tun_note_on[y]; }
            else if (x >= 2 && x <= 13) {
                uint8_t offset = x - 2;
                if (y == tun_track && offset == tun_offset[y])
                    tun_note_on[y] = !tun_note_on[y];  // re-tap toggles note-on
                else {
                    tun_track = y;
                    tun_offset[y] = offset;
                }
            }
            km_tuning_preview();
        }
        else if (y == 5 && x <= 9) {
            tun_octave = x;
            km_tuning_preview();
        }
        else if (y == 5 && (x == 11 || x == 13 || x == 14 || x == 15)) {
            tun_hold_x = x;  // acted on release (short) / after hold (long)
            tun_hold_start = get_ticks();
        }
        else if (y == 6) {
            tuning_set(tun_track, tun_slot(tun_track),
                       (uint16_t)x * (TUNING_DAC_MAX / 16));
            km_tuning_preview();
        }
        else if (y == 7) {
            int16_t delta =
                (x >= 8) ? (int16_t)(1 << (x - 8)) : -(int16_t)(1 << (8 - x));
            uint16_t slot = tun_slot(tun_track);
            if (tun_mod) {
                for (uint8_t t = 0; t < 4; t++)
                    for (uint8_t s = 0; s < TUNING_SLOTS; s++)
                        tuning_set(t, s, tuning_get(t, s) + delta);
            }
            else
                tuning_set(tun_track, slot,
                           tuning_get(tun_track, slot) + delta);
            km_tuning_preview();
        }
    }
    else {  // release
        if (y == 4 && x == 0) { tun_mod = false; }
        else if (y == 5 && x == tun_hold_x) {
            bool long_press =
                (get_ticks() - tun_hold_start) >= KM_TUNING_HOLD_MS;
            km_tuning_util(x, long_press);
            tun_hold_x = 0xff;
        }
    }
}

// Switch the front-most grid view, dropping any preview gates the tuning view
// raised so they don't hang when leaving it.
static void km_switch_view(uint8_t v) {
    if (km_view == KM_VIEW_TUNING && v != KM_VIEW_TUNING) km_tuning_leave();
    km_view = v;
}

void kria_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!kria_owns_grid()) return;
    if (active && km_view == KM_VIEW_TIME)
        km_time_key(x, y, z);
    else if (active && km_view == KM_VIEW_CONFIG)
        km_config_key(x, y, z);
    else if (active && km_view == KM_VIEW_I2C) {
        mode_i2c_view_grid_key(x, y, z);
    }
    else if (active && km_view == KM_VIEW_TUNING)
        km_tuning_key(x, y, z);
    else {
        kria_grid_process_key(&eng, &kgrid, x, y, z);
        if (z) cfg_dirty = true;
    }
    dirty = true;
}

void kria_grid_render(void) {
    if (active && km_view == KM_VIEW_TIME)
        km_time_render();
    else if (active && km_view == KM_VIEW_CONFIG)
        km_config_render();
    else if (active && km_view == KM_VIEW_I2C)
        kria_i2c_view_render(monomeLedBuffer, monome_is_vari());
    else if (active && km_view == KM_VIEW_TUNING)
        km_tuning_render();
    else
        kria_grid_refresh(&eng, &kgrid, monomeLedBuffer, monome_is_vari());
}

// ---- keyboard ----

static void km_set_period(uint16_t p) {
    grid_clock_set_period(&clk, p);
    eng.cfg.clock_period = clk.period;
    cfg_dirty = true;
    if (timer_enabled) kriaClockTimer.ticks = clk.period;
    dirty = true;
}

void process_kria_keys(uint8_t key, uint8_t mod_key, bool is_held_key) {
    if (is_held_key) return;

    // MIDI-follower editor: consumes the key (return) or exits + falls through.
    if (mode_i2c_oled_handle_key(key, mod_key, is_held_key, &dirty)) return;

    if (match_no_mod(mod_key, key, HID_SPACEBAR)) { kria_toggle_run(); }
    else if (match_no_mod(mod_key, key, HID_R)) { kria_engine_reset(&eng); }
    else if (match_no_mod(mod_key, key, HID_X)) {
        grid_clock_set_external(&clk, !clk.external);
    }
    else if (match_no_mod(mod_key, key, HID_UNDERSCORE)) {  // '-' slower
        km_set_period(clk.period + KM_TEMPO_STEP);
    }
    else if (match_no_mod(mod_key, key, HID_PLUS)) {  // '=' faster
        km_set_period(clk.period > KM_TEMPO_STEP ? clk.period - KM_TEMPO_STEP
                                                 : KR_CLOCK_PERIOD_MIN);
    }
    else if (match_no_mod(mod_key, key, HID_S)) {  // explicit save
        kria_flush_if_dirty();
        mode_confirm_show("SAVED");
    }
    else if (match_no_mod(mod_key, key, HID_1)) {  // sequencer view
        km_switch_view(KM_VIEW_SEQ);
    }
    else if (match_no_mod(mod_key, key, HID_2)) {  // time view (Ansible Key 1)
        km_switch_view(KM_VIEW_TIME);
        km_sync_rc_from_period();
    }
    else if (match_no_mod(mod_key, key,
                          HID_3)) {  // config view (Ansible Key 2)
        km_switch_view(KM_VIEW_CONFIG);
    }
    else if (match_no_mod(mod_key, key, HID_4)) {  // i2c follower routing view
        km_switch_view(KM_VIEW_I2C);
        kria_i2c_view_enter();
    }
    else { return; }

    // every handled key repaints both the OLED and the grid
    scene_state.grid.grid_dirty = 1;
    dirty = true;
}

// ---- OLED ----

#define KM_S_LABEL 5
#define KM_S_VALUE 12
#define KM_S_TITLE 15

static const char* const km_page_name[10] = { "TRIG",  "NOTE",   "OCT",
                                              "DUR",   "RPT",    "ALT",
                                              "GLIDE", "SCALE",  "PATT",
                                              "SCRIPT" };
static const char* const km_view_name[5] = { "SEQ", "TIME", "CONFIG", "I2C",
                                             "TUNING" };


// ---- native ops (KR.* retargeted from external-Ansible i2c to the engine)
// ---- All ensure the engine is constructed so ops work even before entering
// the mode. get/set pairs: set != 0 writes val; every op returns the current
// value.

void kria_op_run(int16_t on) {
    km_init_once();
    if (on && !kria_running)
        kria_toggle_run();
    else if (!on && kria_running)
        kria_toggle_run();
}

void kria_op_reset(void) {
    km_init_once();
    kria_engine_reset(&eng);
    dirty = true;
}

int16_t kria_op_pattern(int16_t set, int16_t val) {
    km_init_once();
    if (set) {
        if (val < 0) val = 0;
        if (val >= KRIA_NUM_PATTERNS) val = KRIA_NUM_PATTERNS - 1;
        kria_engine_change_pattern(&eng, (uint8_t)val);
        if (!kgrid.meta_lock) kgrid.edit_pattern = (uint8_t)val;
        cfg_dirty = true;
        dirty = true;
    }
    return eng.cfg.pattern;
}

int16_t kria_op_scale(int16_t set, int16_t val) {
    km_init_once();
    if (set) {
        if (val < 0) val = 0;
        if (val >= MP_SCALE_SLOTS) val = MP_SCALE_SLOTS - 1;
        eng.cfg.p[eng.cfg.pattern].scale = (uint8_t)val;
        km_apply_scale();
        cfg_dirty = true;
        dirty = true;
    }
    return eng.cfg.p[eng.cfg.pattern].scale;
}

int16_t kria_op_period(int16_t set, int16_t val) {
    km_init_once();
    if (set) km_set_period((uint16_t)(val < 1 ? 1 : val));
    return (int16_t)clk.period;
}

int16_t kria_op_mute(int16_t track, int16_t set, int16_t val) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return 0;
    if (set) {
        kria_engine_set_mute(&eng, (uint8_t)track, val ? 1 : 0);
        dirty = true;
    }
    return eng.rt.mutes[track];
}

void kria_op_tmute(int16_t track) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return;
    kria_engine_set_mute(&eng, (uint8_t)track, !eng.rt.mutes[track]);
    dirty = true;
}

void kria_op_clock(int16_t track) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return;
    writing = true;
    kria_engine_clock_track(&eng, (uint8_t)track);
    writing = false;
    dirty = true;
}

int16_t kria_op_dir(int16_t track, int16_t set, int16_t val) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return 0;
    kria_track_t* t = &eng.cfg.p[eng.cfg.pattern].t[track];
    if (set) {
        if (val < 0) val = 0;
        if (val > KR_DIR_RANDOM) val = KR_DIR_RANDOM;
        t->direction = (uint8_t)val;
        cfg_dirty = true;
    }
    return t->direction;
}

int16_t kria_op_cue(int16_t set, int16_t val) {
    km_init_once();
    if (set) {
        if (val < 0) val = 0;
        if (val >= KRIA_NUM_PATTERNS) val = KRIA_NUM_PATTERNS - 1;
        eng.rt.cue_pat_next = (uint8_t)(val + 1);
        dirty = true;
    }
    return eng.rt.cue_pat_next ? (int16_t)(eng.rt.cue_pat_next - 1)
                               : (int16_t)eng.cfg.pattern;
}

int16_t kria_op_pos(int16_t track, int16_t param, int16_t set, int16_t val) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS || param < 0 ||
        param >= KRIA_NUM_PARAMS)
        return 0;
    if (set) {
        eng.rt.pos[track][param] = (uint8_t)(val & 0x0f);
        dirty = true;
    }
    return eng.rt.pos[track][param];
}

int16_t kria_op_loop_start(int16_t track, int16_t param, int16_t set,
                           int16_t val) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS || param < 0 ||
        param >= KRIA_NUM_PARAMS)
        return 0;
    if (set) {
        kria_engine_set_loop_start(&eng, (uint8_t)track, (uint8_t)param,
                                   (uint8_t)val);
        cfg_dirty = true;
        dirty = true;
    }
    return eng.cfg.p[eng.cfg.pattern].t[track].lstart[param];
}

int16_t kria_op_loop_len(int16_t track, int16_t param, int16_t set,
                         int16_t val) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS || param < 0 ||
        param >= KRIA_NUM_PARAMS)
        return 0;
    if (set) {
        kria_engine_set_loop_len(&eng, (uint8_t)track, (uint8_t)param,
                                 (uint8_t)val);
        cfg_dirty = true;
        dirty = true;
    }
    return eng.cfg.p[eng.cfg.pattern].t[track].llen[param];
}

int16_t kria_op_cv(int16_t track) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return 0;
    uint8_t combined = eng.rt.note[track] + eng.rt.alt_note[track];
    uint8_t nis = combined % 7;
    uint8_t ob = combined / 7;
    return (int16_t)((int)eng.rt.cur_scale[nis] + eng.rt.scale_adj[nis] +
                     (int)((eng.rt.oct[track] + ob) * 12));
}

int16_t kria_op_dur(int16_t track) {
    km_init_once();
    if (track < 0 || track >= KRIA_NUM_TRACKS) return 0;
    kria_track_t* t = &eng.cfg.p[eng.cfg.pattern].t[track];
    return t->dur[eng.rt.pos[track][KR_P_DUR]];
}

int16_t kria_op_ii(int16_t follower, int16_t set, int16_t val) {
    km_init_once();
    if (follower < 0 || follower >= KRIA_I2C_FOLLOWERS) return 0;
    if (set) {
        kria_i2c_set_active((uint8_t)follower, val ? 1 : 0);
        mode_flush_i2c_if_dirty();  // persist immediately (global follower
                                    // bank)
        dirty = true;
    }
    return kria_i2c_follower((uint8_t)follower)->active;
}

uint8_t screen_refresh_kria(void) {
    if (!dirty) return 0;
    dirty = false;

    if (mode_i2c_oled_render_active()) return 0b11111111;

    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    const char* cmsg;
    const char* title = mode_confirm_active(&cmsg) ? cmsg : "KRIA";
    font_string_region_clip(&line[0], title, 0, 0, KM_S_TITLE, 0);
    font_string_region_clip(&line[0], km_view_name[km_view], 54, 0, KM_S_VALUE,
                            0);
    font_string_region_clip(&line[0], kria_running ? "RUN" : "STOP", 100, 0,
                            KM_S_VALUE, 0);

    font_string_region_clip(&line[1], "PATT", 0, 0, KM_S_LABEL, 0);
    mode_draw_num(1, 42, eng.cfg.pattern, KM_S_VALUE);
    font_string_region_clip(&line[1], eng.cfg.meta ? "META" : "", 84, 0,
                            KM_S_VALUE, 0);

    font_string_region_clip(&line[2], "CLOCK", 0, 0, KM_S_LABEL, 0);
    font_string_region_clip(&line[2], clk.external ? "EXT" : "INT", 42, 0,
                            KM_S_VALUE, 0);
    mode_draw_num(2, 78, clk.period, KM_S_VALUE);
    font_string_region_clip(&line[2], "MS", 108, 0, KM_S_LABEL, 0);

    font_string_region_clip(&line[3], "TRACK", 0, 0, KM_S_LABEL, 0);
    mode_draw_num(3, 42, kgrid.track, KM_S_VALUE);
    font_string_region_clip(&line[3], "PAGE", 66, 0, KM_S_LABEL, 0);
    font_string_region_clip(&line[3],
                            kgrid.mode < 10 ? km_page_name[kgrid.mode] : "?",
                            102, 0, KM_S_VALUE, 0);

    font_string_region_clip(&line[7], "1SEQ 2TIME 3CFG 4I2C  SPACE:RUN S:SAVE",
                            0, 0, 3, 0);

    return 0b11111111;
}
