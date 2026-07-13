#ifndef _MEADOWPHYSICS_ENGINE_H_
#define _MEADOWPHYSICS_ENGINE_H_

// Meadowphysics sequencer engine, ported from Ansible (src/ansible_grid.c).
//
// A cascading-counter sequencer: MP_ROWS independent counters, each with
// count/speed/min/max. When a counter rolls over it fires its triggers/toggles,
// optionally syncs (resets) other rows, and applies a rule to a destination
// row's count and/or speed.
//
// This engine is hardware-abstracted and self-contained (no globals, no
// libavr32 dependencies): all state lives in mp_engine_t, all output goes
// through the mp_output_t callback vtable, and randomness is injected. That
// makes the counter/rule logic host-unit-testable (see
// tests/meadowphysics_tests.c).
//
// Output binding (voice-mode -> TT CV/TR), the dedicated clock, the mode shell,
// grid UI, OLED screen and persistence live outside this file (later phases).

#include <stdbool.h>
#include <stdint.h>

// MP has 8 counters/rows. This is structural (fixed) and distinct from the
// count/min/max *range*, which is data-driven (uint8_t) and set across the grid
// width by the UI -- a 16-wide grid gives a 16-step range with no engine
// change.
#define MP_ROWS 8

// Editable scale bank: 16 slots x 8 interval steps (step[0]=base 0,
// step[1..7]=semitone deltas fed to mp_engine_calc_scale). Shared by the
// flash store (nvram_data_t) and the grid scale editor.
#define MP_SCALE_SLOTS 16

// Voice modes (how row events map to outputs; applied in mp_note_on/off).
#define MP_1V 0  // mono: 1 note/tick
#define MP_2V 1  // 2 notes/tick, oldest-stolen
#define MP_4V 2  // 4 notes/tick, oldest-stolen
#define MP_8T 3  // 8 gates: rows 0-3 -> TR, rows 4-7 -> CV-as-gate
// MP_SCRIPT: each row n fires Teletype script n+1 on its rising edge (the
// binding maps out_tr(n, 1) -> run_script). No CV/voice allocation; MP owns no
// CV/TR jack, so the triggered scripts drive the outputs themselves.
#define MP_SCRIPT 4
#define MP_VOICE_MODE_COUNT 5  // number of selectable voice modes (for cycling)

// Rules applied to a destination row when the source row rolls over.
#define MP_RULE_NONE 0
#define MP_RULE_INC 1
#define MP_RULE_DEC 2
#define MP_RULE_MAX 3
#define MP_RULE_MIN 4
#define MP_RULE_RND 5
#define MP_RULE_POLE 6
#define MP_RULE_STOP 7

// rule_dest_targets bitmask: which field(s) the rule acts on.
#define MP_TARGET_COUNT 1
#define MP_TARGET_SPEED 2

// Persistent per-scene configuration. This is the only part that gets
// serialized into a scene (~105 B); Ansible's mp_data_t minus glyph[8] (no
// presets on Teletype). Types match Ansible for byte-faithful behavior.
typedef struct {
    uint8_t count[MP_ROWS];       // start position of the countdown
    int8_t speed[MP_ROWS];        // extra ticks per step (0 = fastest)
    uint8_t min[MP_ROWS];         // range floor (used by rules)
    uint8_t max[MP_ROWS];         // range ceiling (used by rules)
    uint8_t trigger[MP_ROWS];     // bitmask: rows to gate high on rollover
    uint8_t toggle[MP_ROWS];      // bitmask: rows to flip on rollover
    uint8_t rules[MP_ROWS];       // MP_RULE_*
    uint8_t rule_dests[MP_ROWS];  // destination row for the rule
    uint8_t sync[MP_ROWS];        // bitmask: rows to reset on rollover
    uint8_t rule_dest_targets[MP_ROWS];  // MP_TARGET_* bitmask
    uint8_t smin[MP_ROWS];               // speed range floor (for rules)
    uint8_t smax[MP_ROWS];               // speed range ceiling (for rules)
    uint8_t scale;                       // index into TT's scale system (A4)
    uint8_t voice_mode;                  // MP_1V/2V/4V/8T
    uint8_t sound;                       // manual-play mode enabled
} mp_config_t;

// Ephemeral runtime state -- never serialized into a scene (Phase 0 rule).
typedef struct {
    int8_t position[MP_ROWS];  // current position; -1 = stopped
    uint8_t tick[MP_ROWS];     // speed countdown
    uint8_t pushed[MP_ROWS];   // manual-play push pending (sound mode)
    uint8_t reset[MP_ROWS];    // pending re-arm this tick
    uint8_t state[MP_ROWS];    // current gate state
    uint8_t pstate[MP_ROWS];   // gate state at start of tick (edge detect)
    uint8_t clear[MP_ROWS];    // trigger to be cleared on next phase-0
    uint16_t mp_clock_count;   // notes emitted this tick (voice cap)
    int8_t note_now[4];        // voice allocation: row currently in each voice
    uint16_t note_age[4];      // age for oldest-note stealing
    uint8_t cur_scale[8];  // absolute scale degrees (from mp_engine_calc_scale)
    int8_t scale_adj[8];   // per-degree adjust (parity; 0 by default)
} mp_runtime_t;

// Thin output interface. The engine never touches hardware directly.
//   tr(ctx, ch, on)      -- trigger output ch high/low        (all modes)
//   cv(ctx, ch, note)    -- CV output ch to a scale-degree note (1V/2V/4V)
//   cv_gate(ctx, ch, on) -- CV output ch to full-scale/0 as a gate (8T)
// `note` is a scale degree (cur_scale[..] + scale_adj[..]); conversion to a
// calibrated DAC value is the binding layer's job (Phase 2).
typedef struct {
    void (*tr)(void* ctx, uint8_t ch, uint8_t on);
    void (*cv)(void* ctx, uint8_t ch, int16_t note);
    void (*cv_gate)(void* ctx, uint8_t ch, uint8_t on);
    void* ctx;
} mp_output_t;

typedef struct {
    mp_config_t cfg;
    mp_runtime_t rt;
    mp_output_t out;
    uint32_t (*rnd)(void* ctx);  // injected RNG for MP_RULE_RND
    void* rnd_ctx;
} mp_engine_t;

// Fill cfg with Ansible's default_mp() pattern (ascending counters, each row
// triggers and syncs itself). Range values assume a 16-wide grid.
void mp_engine_set_defaults(mp_config_t* cfg);

// True if every field is within the valid range (all are used as array
// indices). Persisted/stale config that fails this must be replaced with
// defaults before use to prevent out-of-bounds indexing.
bool mp_engine_config_valid(const mp_config_t* cfg);

// Generic indexed config accessor for the KR.MP / MP.CFG script ops. See the
// definition for the field map (0 count .. 11 smax); clamps keep the config
// valid. `lane_mask` confines the mask fields to the live lanes, `max_dest`
// bounds the rule destination. Returns the resulting field value.
int16_t mp_config_field(mp_config_t* cfg, uint8_t row, uint8_t field,
                        uint8_t set, int16_t val, uint8_t lane_mask,
                        uint8_t max_dest);

// Initialize the engine: bind outputs + RNG, load defaults, arm all rows.
void mp_engine_init(mp_engine_t* e, const mp_output_t* out,
                    uint32_t (*rnd)(void* ctx), void* rnd_ctx);

// Re-arm all rows (position = count, tick = speed) and clear voice allocation.
// Mirrors init_mp()'s runtime reset without reloading cfg.
void mp_engine_reset(mp_engine_t* e);

// Re-arm a single row (MP.RESET n behavior).
void mp_engine_reset_row(mp_engine_t* e, uint8_t row);

// Stop all rows / a single row (position = -1). MP.STOP behavior.
void mp_engine_stop(mp_engine_t* e);
void mp_engine_stop_row(mp_engine_t* e, uint8_t row);

// Queue a manual push of a row (sound mode): fires its triggers/sync on the
// next phase-1 clock, independent of the countdown.
void mp_engine_push(mp_engine_t* e, uint8_t row);

// Advance the sequencer. phase != 0 is the "on" edge (counters step, notes
// fire); phase == 0 is the "off" edge (queued trigger clears / note-offs).
// Direct port of Ansible clock_mp().
void mp_engine_clock(mp_engine_t* e, uint8_t phase);

// Recompute rt.cur_scale from 8 interval steps (semitone deltas), matching
// Ansible calc_scale(): cur_scale[0]=intervals[0]; cur_scale[i]+=interval.
void mp_engine_calc_scale(mp_engine_t* e, const uint8_t intervals[8]);

#endif
