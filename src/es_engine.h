#ifndef _ES_ENGINE_H_
#define _ES_ENGINE_H_

// Earthsea engine, ported from Ansible v3.2 (src/ansible_grid.c, the ES app).
//
// A live polyphonic grid instrument plus phrase recorder. The grid is a
// fourths-layout keyboard (semitone = x + (rows-1-y)*5 - 1, clamped 0..119);
// up to 4 voices are allocated "reuse same key, else first free, else steal
// oldest". A pattern records up to ES_EVENTS_PER_PATTERN key on/off events,
// each with the interval since the previous event in ms ticks; playback
// re-emits them on the same timeline. Per-pattern transforms: reverse,
// double/half speed, linearize; edge modes shape gates (as-recorded / fixed
// length / drone toggle). Arp mode re-roots the pattern from live key presses.
//
// Like the Kria/MP engines this is hardware-abstract and self-contained: all
// persistent data lives in es_config_t, ephemeral state in es_runtime_t, all
// output goes through the es_output_t vtable. Unlike Kria (fixed-period
// clock), Earthsea is *timer-driven with variable intervals* and the engine
// does not read a clock: every entry point that needs wall time takes a
// `now` argument (ms ticks), and playback stepping returns the interval to
// the next event so the caller (mode shell) can arm a one-shot timer. Fixed
// edge mode is handled the same way: note_on carries a duration and the
// shell/binding owns the note-off timer, calling es_engine_note_off_voice()
// when it fires. See EARTHSEA_PORT_PLAN.md §3/§7.

#include <stdbool.h>
#include <stdint.h>

#define ES_NUM_PATTERNS 16
#define ES_EVENTS_PER_PATTERN 128
#define ES_NUM_VOICES 4

// Events closer together than this (ms ticks) are one chord; also the pivot
// for the double/half speed and linearize transforms (Ansible value).
#define ES_CHORD_THRESHOLD 30

// Gate shaping per pattern (es_pattern_t.edge). Values match Ansible.
#define ES_EDGE_PATTERN 0  // gates exactly as recorded
#define ES_EDGE_FIXED 1    // every gate lasts edge_time ticks
#define ES_EDGE_DRONE 2    // presses toggle; pattern note-offs ignored

// Grid keyboard geometry: 16x8 primary (column 0 is the control strip; the
// playable field is x 1..15). Ansible's 256 support (keymap[256], bottom-half
// views) is deliberately dropped -- see EARTHSEA_PORT_PLAN.md §1.
#define ES_KEYMAP_SIZE 128

// One recorded key event. `on`: 0 = off, 1 = on, 2 = rest-off, 3 = rest-on
// (rests are recorded key (15,0) presses -- timeline spacers that play
// nothing). `index` = x + (y << 4). `interval` = ms ticks to the *next* event.
typedef struct {
    uint8_t on;
    uint8_t index;
    uint16_t interval;
} es_event_t;

typedef struct {
    es_event_t e[ES_EVENTS_PER_PATTERN];
    uint16_t interval_ind;  // index of the first supra-threshold interval
    uint16_t length;        // recorded event count
    uint8_t loop;
    uint8_t root_x;  // pattern root (transpose reference), grid coords
    uint8_t root_y;
    uint8_t edge;        // ES_EDGE_*
    uint16_t edge_time;  // fixed-edge gate length, ticks (16..256)
    uint8_t voices;      // playback voice mask (bit n = voice n)
    uint8_t dir;         // 0 = forward, 1 = reverse (event list is rewritten)
    uint8_t linearize;   // all intervals -> the interval at interval_ind
    uint8_t start;       // reserved (Ansible loop-range remnant; kept for
    uint8_t end;         //  layout fidelity, always 0/15)
} es_pattern_t;

// The single global Earthsea bank persisted in NVRAM (one instance; Ansible's
// 8 grid presets + glyphs are dropped). ~8.6 KB -- see EARTHSEA_PORT_PLAN.md
// §0.
typedef struct {
    uint8_t arp;       // arp mode: live presses re-root the playing pattern
    uint8_t p_select;  // active pattern 0..15
    uint8_t voices;    // live-play voice mask
    uint8_t octave;    // live-play octave shift 0..5
    uint8_t scale;     // scale display overlay: 0..15 into the shared scale
                       // bank, 16 = off (shows keymap shading instead)
    uint8_t keymap[ES_KEYMAP_SIZE];  // per-key shading 0..2 (visual guide)
    es_pattern_t p[ES_NUM_PATTERNS];
} es_config_t;

typedef enum {
    es_stopped,
    es_armed,  // first key press starts recording
    es_recording,
    es_playing,
} es_mode_t;

// One allocated voice. `active`, grid coords (signed: arp transposition can
// push them off-grid; pitch math wraps by fourths), allocation timestamp for
// oldest-steal, and whether the note came from pattern playback (pattern
// stop kills only those).
typedef struct {
    uint8_t active;
    int8_t x;
    int8_t y;
    uint32_t start;
    uint8_t from_pattern;
} es_note_t;

// Ephemeral runtime state -- never serialized.
typedef struct {
    es_mode_t mode;
    es_note_t notes[ES_NUM_VOICES];
    uint16_t pos;            // playback event index
    uint32_t rec_tick;       // recording: timestamp of the previous event
    uint32_t p_start;        // playback: timestamp of pattern start (UI/scrub)
    uint32_t p_total;        // playback: total pattern time (linearize-aware)
    uint8_t clock_external;  // set by the shell; suppresses timer-driven
                             // playback (start_playback returns 0, stepping
                             // happens via es_engine_clock_step)
} es_runtime_t;

// Thin output interface; the engine never touches hardware.
//   note_on(ctx, voice, semitones, duration) -- start voice: pitch as a
//     0..119 semitone index (binding -> ET), gate high. duration != 0 (fixed
//     edge): the caller must schedule a note-off in `duration` ticks and then
//     call es_engine_note_off_voice(). duration == 0: gate until note_off.
//   note_off(ctx, voice) -- gate low.
typedef struct {
    void (*note_on)(void* ctx, uint8_t voice, int16_t semitones,
                    uint16_t duration);
    void (*note_off)(void* ctx, uint8_t voice);
    void* ctx;
} es_output_t;

typedef struct {
    es_config_t cfg;
    es_runtime_t rt;
    es_output_t out;
} es_engine_t;

// Fill cfg with Ansible's default_es() values (empty patterns, all voices,
// scale off). (Ansible's own default_es has an i/j index slip; this writes
// what it *meant*: every pattern initialized.)
void es_engine_set_defaults(es_config_t* cfg);

// True if every index-critical field is in range (lengths, p_select, edge,
// interval_ind, voice masks, octave, scale, keymap values). Stale/invalid
// persisted config must be replaced with defaults before use.
bool es_engine_config_valid(const es_config_t* cfg);

// Initialize: bind outputs, reset runtime (mode = stopped, voices silent).
// cfg is left untouched -- load or default it separately.
void es_engine_init(es_engine_t* e, const es_output_t* out);

// --- live keyboard ------------------------------------------------------

// Grid semitone mapping (fourths layout), exposed for the binding/tests.
int16_t es_engine_note_index(int8_t x, int8_t y);

// A playable-field key event (x 1..15). Handles, in order: armed -> start
// recording; recording -> append event; arp -> re-root + restart playback
// (returns the first interval like es_engine_start_playback); keymap editing
// (rest_held while stopped); otherwise live note on/off (drone edge toggles).
// Returns the ticks to the next playback event when it (re)starts playback,
// else 0. `now` = current ms ticks.
uint32_t es_engine_grid_press(es_engine_t* e, uint8_t x, uint8_t y, uint8_t z,
                              bool rest_held, uint32_t now);

// Kill every active voice (note_off + deallocate) / only pattern-born voices.
void es_engine_kill_all_notes(es_engine_t* e);
void es_engine_kill_pattern_notes(es_engine_t* e);

// End one voice: emit note_off + deallocate. This is what the shell's
// fixed-edge duration timer calls when it fires (Ansible es_note_off_i).
void es_engine_note_off_voice(es_engine_t* e, uint8_t voice);

// --- transport ----------------------------------------------------------

// Arm (next press records), or complete/stop recording. Mirrors the Ansible
// arm-key state machine (minus the held-key release quirks, which the grid
// layer owns).
void es_engine_arm(es_engine_t* e);
void es_engine_stop_recording(es_engine_t* e, uint32_t now);

// Start playback at scrub position pos/16 (0 = start). Completes a pending
// recording first. Returns the ticks until the next event (arm a one-shot
// timer), or 0 if there is nothing to play. Internal-clock mode only; under
// external clock just call es_engine_clock_step().
uint32_t es_engine_start_playback(es_engine_t* e, uint8_t pos, uint32_t now);

// Advance past the current event: emit it (and every chord-grouped follower),
// then return the ticks to the next event, or 0 when playback ended (non-loop
// pattern exhausted or pattern empty). The shell re-arms its timer with the
// return value.
uint32_t es_engine_play_advance(es_engine_t* e, uint32_t now);

// External-clock step: play the next chord group in one shot (the Ansible
// handler_ESTr input-1 walk). No timers involved.
void es_engine_clock_step(es_engine_t* e, uint32_t now);

void es_engine_stop_playback(es_engine_t* e);

// --- pattern ops (grid views / ES.* ops) ---------------------------------

// Select a pattern (does not start playback; pair with start_playback).
void es_engine_set_pattern(es_engine_t* e, uint8_t pattern);

// Runes / ES.MAGIC transforms; all operate on the selected pattern.
void es_engine_double_speed(es_engine_t* e);
void es_engine_half_speed(es_engine_t* e);
void es_engine_set_linearize(es_engine_t* e, uint8_t on);
void es_engine_set_direction(es_engine_t* e, uint8_t rev);  // reverses in place

// Walk the pattern root +/- steps along the fourths layout (ES.TRANS).
void es_engine_transpose(es_engine_t* e, int16_t delta);

// Set edge mode; fixed edge_time in ticks is clamped to 16..256 (ES.MODE).
void es_engine_set_edge(es_engine_t* e, uint8_t edge, uint16_t edge_time);

#endif
