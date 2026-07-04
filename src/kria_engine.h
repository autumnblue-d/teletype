#ifndef _KRIA_ENGINE_H_
#define _KRIA_ENGINE_H_

// Kria sequencer engine, ported from Ansible (src/ansible_grid.c).
//
// Four independent tracks. Each track has KRIA_NUM_PARAMS parameter "loops"
// (trigger, note, octave, duration, repeat, alt-note, glide), each an
// independent up-to-16-step loop with its own start/end/length and per-param
// clock divider (tmul). A pattern = 4 tracks + a scale; a "song" holds
// KRIA_NUM_PATTERNS patterns plus a meta-sequencer that chains them.
//
// This engine is hardware-abstract and self-contained (no globals, no libavr32
// dependencies): all persistent data lives in kria_config_t, all ephemeral
// state in kria_runtime_t, all output goes through the kria_output_t vtable, and
// randomness is injected. That makes the stepping/loop/meta/scale logic
// host-unit-testable (see tests/kria_tests.c).
//
// Deliberate split vs Ansible: Ansible drives gate length and repeat spacing
// with real-tick softTimers (get_ticks/timer_add). Here the engine only makes
// sequencing *decisions* and emits the first note of each trigger; it records
// the nominal duration/repeat parameters (rt.dur_unscaled/rpt/rptBits) and
// exposes kria_engine_note_off()/kria_engine_repeat() as callable functions.
// The mode shell (Phase 2/4) owns the timers, does the real-tick scaling
// (clock_deltas * tmul / 384, dur_mul<<2, rptTicks), and calls those back. The
// binding layer maps the emitted semitone index to a calibrated DAC value (ET).

#include <stdbool.h>
#include <stdint.h>

#define KRIA_NUM_TRACKS 4
#define KRIA_NUM_PARAMS 7
// Scenario B (Full Kria): Ansible's native 16 patterns per song. kria_config_t
// ~18.5 KB; funded by SCENE_SLOTS 30 -> 20. See KRIA_PORT_PLAN.md §0.
#define KRIA_NUM_PATTERNS 16

// Per-track parameter loop indices (Ansible kria_modes_t, the 7 looped params).
#define KR_P_TR 0
#define KR_P_NOTE 1
#define KR_P_OCT 2
#define KR_P_DUR 3
#define KR_P_RPT 4
#define KR_P_ALTNOTE 5
#define KR_P_GLIDE 6

// Track playback direction (kria_track_t.direction).
#define KR_DIR_FORWARD 0
#define KR_DIR_REVERSE 1
#define KR_DIR_TRIANGLE 2
#define KR_DIR_DRUNK 3
#define KR_DIR_RANDOM 4

// Sync mode: when/how per-param divider (tmul) edits take effect.
#define KR_SYNC_NONE 0x00
#define KR_SYNC_TIMEDIV 0x01

typedef struct {
    uint8_t tr[16];
    int8_t oct[16];
    uint8_t note[16];
    uint8_t dur[16];
    uint8_t rpt[16];
    uint8_t rptBits[16];
    uint8_t alt_note[16];
    uint8_t glide[16];

    uint8_t p[KRIA_NUM_PARAMS][16];  // per-step probability 0..3

    uint8_t dur_mul;
    uint8_t direction;  // KR_DIR_*, stored as u8 to pin the size to 1 byte
    uint8_t advancing[KRIA_NUM_PARAMS];  // triangle-mode direction latch (mutated)
    uint8_t octshift;

    uint8_t lstart[KRIA_NUM_PARAMS];
    uint8_t lend[KRIA_NUM_PARAMS];
    uint8_t llen[KRIA_NUM_PARAMS];
    uint8_t lswap[KRIA_NUM_PARAMS];  // engine-unused (UI/loop-edit only)
    uint8_t tmul[KRIA_NUM_PARAMS];   // per-param clock divider (>=1)

    uint8_t tt_clocked;       // advanced by TT/i2c KR.CLK instead of internal clock
    uint8_t trigger_clocked;  // value params advance only when a trigger fires
} kria_track_t;

typedef struct {
    kria_track_t t[KRIA_NUM_TRACKS];
    uint8_t scale;  // index into the shared scale bank
} kria_pattern_t;

// The single global Kria "song" persisted in NVRAM (one instance, not 8).
typedef struct {
    kria_pattern_t p[KRIA_NUM_PATTERNS];
    uint8_t pattern;  // active pattern index
    uint8_t meta_pat[64];
    uint8_t meta_steps[64];
    uint8_t meta_start;
    uint8_t meta_end;
    uint8_t meta_len;
    uint8_t meta_lswap;
    uint8_t glyph[8];

    // Engine/song-level settings (Ansible kria_state_t scalars). Kept in the
    // persisted config so a saved song restores its behavior.
    uint8_t sync_mode;       // KR_SYNC_*
    uint8_t cue_div;         // cue sub-step divider (0 = every clock)
    uint8_t cue_steps;       // clocks per cue step (0-based)
    uint8_t meta;            // meta-sequencer enabled
    uint8_t meta_reset_all;  // reset input also resets the meta pointer
    uint8_t dur_tie_mode;    // hold gate on max-duration steps (legato/tie)
    uint16_t clock_period;   // internal tempo (ms); used by the clock layer
} kria_config_t;

// Ephemeral runtime state -- never serialized.
typedef struct {
    uint8_t pos[KRIA_NUM_TRACKS][KRIA_NUM_PARAMS];      // current step per param
    uint8_t pos_mul[KRIA_NUM_TRACKS][KRIA_NUM_PARAMS];  // divider sub-counter
    uint8_t tmul_live[KRIA_NUM_TRACKS][KRIA_NUM_PARAMS];  // live divider target

    // latched per-step values (updated when their param advances)
    uint8_t note[KRIA_NUM_TRACKS];
    uint8_t oct[KRIA_NUM_TRACKS];
    uint8_t alt_note[KRIA_NUM_TRACKS];
    uint8_t glide[KRIA_NUM_TRACKS];
    uint16_t dur_unscaled[KRIA_NUM_TRACKS];  // (dur[step]+1)*(dur_mul<<2)
    uint8_t rpt[KRIA_NUM_TRACKS];
    uint8_t rptBits[KRIA_NUM_TRACKS];
    uint8_t tr[KRIA_NUM_TRACKS];  // current gate state (for UI / edge)

    // repeat bookkeeping (shell schedules the timing; engine tracks the count)
    uint8_t activeRpt[KRIA_NUM_TRACKS];
    int16_t repeats[KRIA_NUM_TRACKS];

    uint8_t mutes[KRIA_NUM_TRACKS];

    // derived scale (rebuilt by kria_engine_calc_scale)
    uint8_t cur_scale[8];
    int8_t scale_adj[8];

    // clock / cue / meta bookkeeping
    uint32_t clock_count;
    uint8_t cue_count;
    uint8_t cue_sub_count;
    uint8_t meta_pos;
    uint16_t meta_count;
    uint8_t meta_next;      // queued meta jump (1-based; 0 = none)
    uint8_t cue_pat_next;   // queued pattern change (1-based; 0 = none)
    bool pos_reset;         // re-arm all positions on next clock
    bool meta_reset;        // reset meta pointer on next clock
} kria_runtime_t;

// Thin output interface. The engine never touches hardware directly.
//   tr(ctx, ch, on)        -- gate output ch high/low
//   cv(ctx, ch, semitones) -- pitch CV as a 0..120 semitone index (binding -> ET)
//   cv_slew(ctx, ch, slew) -- portamento/slew amount for ch (glide)
typedef struct {
    void (*tr)(void* ctx, uint8_t ch, uint8_t on);
    void (*cv)(void* ctx, uint8_t ch, int16_t semitones);
    void (*cv_slew)(void* ctx, uint8_t ch, uint16_t slew);
    void* ctx;
} kria_output_t;

typedef struct {
    kria_config_t cfg;
    kria_runtime_t rt;
    kria_output_t out;
    uint32_t (*rnd)(void* ctx);  // injected RNG (direction/probability)
    void* rnd_ctx;
    // Caller-owned scale bank (16 x 8 intervals), Ansible's scale_data. The
    // engine recomputes cur_scale from it on pattern/scale change. May be NULL,
    // in which case scale recompute is a no-op (call kria_engine_calc_scale
    // directly instead).
    const uint8_t (*scale_data)[8];
} kria_engine_t;

// Fill cfg with Ansible's default_kria() values (see KRIA_PORT_PLAN.md §7).
void kria_engine_set_defaults(kria_config_t* cfg);

// True if every index-critical field is in range (pattern/loop bounds,
// direction, tmul>=1, rpt>=1, meta pointers). Stale/invalid persisted config
// must be replaced with defaults before use.
bool kria_engine_config_valid(const kria_config_t* cfg);

// Initialize: bind outputs + RNG + scale bank, then re-arm
// (kria_engine_reset). scale_data may be NULL (see the struct field).
void kria_engine_init(kria_engine_t* e, const kria_output_t* out,
                      uint32_t (*rnd)(void* ctx), void* rnd_ctx,
                      const uint8_t (*scale_data)[8]);

// Recompute rt.cur_scale from 8 interval steps (semitone deltas), matching
// Ansible calc_scale(): cur_scale[0]=intervals[0]; cur_scale[i]+=interval.
void kria_engine_calc_scale(kria_engine_t* e, const uint8_t intervals[8]);

// Re-arm all positions to loop-end (so the next clock lands on loop-start) and
// clear cue/clock counters. Mirrors Ansible's pos_reset handling.
void kria_engine_reset(kria_engine_t* e);

// Advance the sequencer one clock. Acts only on the rising edge (phase != 0),
// matching Ansible clock_kria(): cue/meta bookkeeping, pending resets, then
// clocks every track whose tt_clocked is false.
void kria_engine_clock(kria_engine_t* e, uint8_t phase);

// Advance a single track (for tt_clocked tracks driven by TT/i2c KR.CLK).
void kria_engine_clock_track(kria_engine_t* e, uint8_t track);

// Timer callbacks the shell invokes: end a track's gate (respecting
// dur_tie_mode) / fire the next scheduled repeat.
void kria_engine_note_off(kria_engine_t* e, uint8_t track);
void kria_engine_repeat(kria_engine_t* e, uint8_t track);

// Switch the active pattern (forces a position reset + scale recompute request).
void kria_engine_change_pattern(kria_engine_t* e, uint8_t pattern);

// Mute/unmute a track (muted tracks advance but emit no gate/CV).
void kria_engine_set_mute(kria_engine_t* e, uint8_t track, uint8_t mute);

// Set a (track, param) loop start / length (0..15 / 1..16), recomputing lend +
// lswap wrap. For the KR.L.ST / KR.L.LEN ops.
void kria_engine_set_loop_start(kria_engine_t* e, uint8_t track, uint8_t param,
                                uint8_t start);
void kria_engine_set_loop_len(kria_engine_t* e, uint8_t track, uint8_t param,
                              uint8_t len);

#endif
