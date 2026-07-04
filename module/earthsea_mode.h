#ifndef _EARTHSEA_MODE_H_
#define _EARTHSEA_MODE_H_

#include <stdbool.h>
#include <stdint.h>

// Native Earthsea mode shell: owns the engine + grid instances, drives the
// variable-interval play timer (absolute-deadline re-arm) and the per-voice
// fixed-edge note-off timers, binds output (jacks + shared i2c followers),
// and renders the OLED. See EARTHSEA_PORT_PLAN.md §5-§7.
//
// Unlike Kria there is no separate "running" flag: the engine is engaged
// while the view is front-most or a pattern is playing (playback survives
// leaving the mode; exiting kills live/drone notes if nothing is playing).

// Trigger inputs (0-indexed). MP owns 0, Kria owns 1.
#define ES_EXT_CLOCK_INPUT 2  // stepped playback while clock_external
#define ES_PLAY_INPUT 3       // rising edge (re)starts playback

// Enter/leave the Earthsea view (from set_mode). Exit persists edits and
// releases keyboard/grid; a playing pattern keeps playing in the background.
void set_earthsea_mode(void);
void earthsea_mode_exit(void);

// Keyboard handler (from process_keypress).
void process_earthsea_keys(uint8_t key, uint8_t mod_key, bool is_held_key);

// OLED refresh (from handler_ScreenRefresh); returns a dirty bitmask.
uint8_t screen_refresh_earthsea(void);

// Event-loop services dispatched from handler_AppCustom:
//   data == 3       -> es_service_play (play-timer tick)
//   data == 30..33  -> es_service_note_off(voice) (fixed-edge gate end)
void es_service_play(void);
void es_service_note_off(uint8_t voice);

// Trigger-input edges from handler_Trigger; `level` is the pin state.
// Each returns true if Earthsea consumed the edge.
bool es_external_clock(uint8_t level);
bool es_play_trigger(uint8_t level);

// Output ownership: true when a script write to CV/TR channel `ch` (0-3)
// must be suppressed (ES engaged, ch is an enabled voice, not our own write).
bool es_suppresses_output(uint8_t ch);

// Grid ownership + surface (from grid.c).
bool es_owns_grid(void);
void es_grid_key(uint8_t x, uint8_t y, uint8_t z);
void es_grid_render(void);

#endif
