#ifndef _MODE_PERSIST_H_
#define _MODE_PERSIST_H_

#include <stdbool.h>

// Shared save-UX helpers for the grid modes (Meadowphysics, Kria, Earthsea).
// The three modes had divergent save gestures, no visible confirmation, and
// different durability (per-scene vs global bank flushed only on mode exit).
// This unifies the *interaction contract*; storage scope is unchanged.
// See MODE_PERSIST_PLAN.md.

// ---- transient on-screen "SAVED" confirmation ----------------------------
// The firmware had no notification mechanism; this is the single one. A mode's
// screen_refresh queries mode_confirm_active() and draws the message in its
// title cell; the mode's periodic timer calls mode_confirm_tick() so the
// banner self-clears after a short delay.
void mode_confirm_show(const char* msg);
bool mode_confirm_active(const char** out_msg);
bool mode_confirm_tick(void);  // true if it just expired (request a redraw)

// ---- universal scene-save commit -----------------------------------------
// Flush every dirty *global* mode bank (Kria + Earthsea, incl. their shared
// scale/i2c banks) so a scene save persists everything regardless of which
// mode is active. Called from the interactive scene-save sites. Silent: the
// scene-save UI provides its own feedback.
void mode_persist_flush_all_dirty(void);

// Per-mode flush wrappers, defined in each mode (they touch file-static state).
// Each writes only the subsystems whose dirty flag is set; returns true if it
// persisted anything. Also called directly by each mode's exit / save key.
bool kria_flush_if_dirty(void);
bool earthsea_flush_if_dirty(void);

#endif
