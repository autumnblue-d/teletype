#ifndef _MODE_PERSIST_H_
#define _MODE_PERSIST_H_

#include <stdbool.h>
#include <stdint.h>

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

// Flush the shared i2c MIDI-follower bank to flash if it was edited. The bank
// is global (Kria owns it; MP and Earthsea share it), so all three modes flush
// it through this single path. Returns true if it wrote.
bool mode_flush_i2c_if_dirty(void);

// Per-mode flush wrappers, defined in each mode (they touch file-static state).
// Each writes only the subsystems whose dirty flag is set; returns true if it
// persisted anything. Also called directly by each mode's exit / save key.
bool kria_flush_if_dirty(void);
bool earthsea_flush_if_dirty(void);

// ---- shared MIDI-follower editor / i2c-view glue -------------------------
// The i2c follower editor (kria_i2c_oled) and the grid i2c view are shared by
// all three grid modes; these wrap the identical plumbing each mode repeated.

// Call at the top of a mode's key handler. If the follower editor owns the
// keyboard, route the key to it: returns true if the mode should return now
// (key consumed). Returns false -- after leaving the editor -- to fall through
// to normal handling. Sets *dirty when a redraw is needed.
bool mode_i2c_oled_handle_key(uint8_t key, uint8_t mod_key, uint8_t is_held,
                              bool* dirty);

// Call at the top of a mode's screen_refresh. Renders the follower editor if it
// owns the screen; returns true if it did (caller then returns the all-regions
// dirty mask).
bool mode_i2c_oled_render_active(void);

// The grid i2c-follower view's key handler: forward to the shared view and, on
// press, enter the follower editor if the view requested it.
void mode_i2c_view_grid_key(uint8_t x, uint8_t y, uint8_t z);

// Draw an integer at OLED region line `ln`, x-pixel `x`, fg brightness `fg`.
void mode_draw_num(uint8_t ln, uint8_t x, int val, uint8_t fg);

#endif
