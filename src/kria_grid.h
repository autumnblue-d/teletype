#ifndef _KRIA_GRID_H_
#define _KRIA_GRID_H_

// Kria grid surface: key handling + LED rendering, ported from Ansible's
// refresh_kria_view + handler_KriaGridKey (ansible_grid.c). Hardware-abstract
// and testable: operates on the engine (kria_engine_t) plus a small grid
// interaction state, writing into a raw 16x8 LED buffer (index y*16+x). The
// mode shell (Phase 4) routes the monome grid here when Kria owns it and blits
// into monomeLedBuffer.
//
// Scope vs Ansible: this renders/edits the primary 16x8 view. The 256-grid
// second view (rows 8-15) and the OLED preset/clock/config/tuning screens are
// out of scope here (shell/keyboard concern). The mPattern long-press copy
// gesture IS ported (see hold_pending / kria_grid_pattern_hold_fire); the mRpt
// long-press reset is not (its parent row was repurposed for decrement).
// Persisted edit-behavior flags (note_sync/loop_sync/div_sync/note_div_sync)
// live in the grid state with Ansible defaults for now.

#include <stdint.h>

#include "kria_engine.h"

// Grid modes: 0..6 are the per-step param pages and equal the KR_P_* indices
// (mTr..mGlide); 7/8 are the scale/pattern pages.
#define KR_MODE_SCALE 7
#define KR_MODE_PATTERN 8
// Second sub-tab of the DUR page: the 6-lane script-trigger sequencer.
#define KR_MODE_SCRIPTSEQ 9

// Mod-mode overlays.
#define KR_MOD_NONE 0
#define KR_MOD_LOOP 1
#define KR_MOD_TIME 2
#define KR_MOD_PROB 3

typedef struct {
    uint8_t mode;          // KR_P_* (0..6) or KR_MODE_SCALE/PATTERN
    uint8_t mod_mode;      // KR_MOD_*
    uint8_t track;         // edit track 0..3
    uint8_t edit_pattern;  // pattern edited when meta_lock; else follows play

    // meta editing
    uint8_t meta_edit;
    uint8_t meta_lock;
    uint8_t cue;  // momentary (pattern button held): cue/meta edit modifier

    // loop-edit two-press gesture
    uint8_t loop_count;
    uint8_t loop_first;
    int8_t loop_last;
    uint8_t loop_edit;  // track row for the mTr per-track loop gesture

    // mPattern long-press gesture (Ansible grid_keytimer_kria): a plain
    // pattern-select press is deferred -- a quick release switches; holding past
    // the shell's threshold copies the playing pattern into the slot, then
    // switches. Set on press, cleared on switch / fire / page change.
    uint8_t hold_pending;
    uint8_t hold_x;  // pattern slot pressed while a hold is pending

    // blink flags, toggled by the shell's 100 ms timers
    uint8_t alt_blink;
    uint8_t meta_lock_blink;
    uint8_t blinks[KRIA_NUM_TRACKS];  // per-track trigger blink

    // edit-behavior flags (Ansible kria_state_t; defaults in state_init)
    uint8_t note_sync;      // couple tr<->note editing + loops
    uint8_t loop_sync;      // loop fan-out: 0=param, 1=track, 2=all tracks
    uint8_t div_sync;       // tmul fan-out: 0=param, 1=track, 2=all tracks
    uint8_t note_div_sync;  // couple tr/note (and group the rest) for tmul

    // mutable scale bank (caller-owned, 16x8); set by the shell for the scale
    // page. May be NULL (scale-interval editing then no-ops).
    uint8_t (*scale_bank)[8];
} kria_grid_state_t;

void kria_grid_state_init(kria_grid_state_t* g);

// Handle a grid key at (x,y), z=press(1)/release(0). Mutates engine cfg/rt and
// grid state per the current page/mod-mode.
void kria_grid_process_key(kria_engine_t* e, kria_grid_state_t* g, uint8_t x,
                           uint8_t y, uint8_t z);

// Render the current page into `led` (16*8 = 128 bytes, index y*16+x). Clears
// first, clamps to 15. If `vari` is 0 (non-varibright), any lit cell is forced
// to full brightness so dim ramps don't vanish.
void kria_grid_refresh(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led,
                       uint8_t vari);

// Fire the deferred mPattern long-press: if a hold is pending, copy the playing
// pattern into the held slot and switch to it. Returns 1 if it fired, 0 if no
// hold was pending (already released/switched or page changed). The shell calls
// this via an AppCustom event once its hold threshold elapses.
uint8_t kria_grid_pattern_hold_fire(kria_engine_t* e, kria_grid_state_t* g);

#endif
