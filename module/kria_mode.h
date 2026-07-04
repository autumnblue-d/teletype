#ifndef _KRIA_MODE_H_
#define _KRIA_MODE_H_

#include <stdbool.h>
#include <stdint.h>

// Native Kria mode shell: owns the engine + clock + grid instances, drives the
// clock timer and the per-track note-off/repeat/blink timers, binds output, and
// renders the OLED. See KRIA_PORT_PLAN.md §5-§7.

// Trigger input (0-indexed) carrying Kria's external clock when enabled.
#define KR_EXT_CLOCK_INPUT 1

// Enter/leave the Kria view (from set_mode). The engine keeps running in the
// background after exit; exit only relinquishes keyboard/grid + persists.
void set_kria_mode(void);
void kria_mode_exit(void);

// Play/pause the engine (Space in-mode, or alt-K enters the mode). While running
// Kria owns the CV/TR of its un-muted tracks; stopping releases them.
void kria_toggle_run(void);

// Keyboard handler (from process_keypress).
void process_kria_keys(uint8_t key, uint8_t mod_key, bool is_held_key);

// OLED refresh (from handler_ScreenRefresh); returns a dirty bitmask.
uint8_t screen_refresh_kria(void);

// Event-loop services dispatched from handler_AppCustom:
//   data == 2       -> kria_clock_tick (internal clock)
//   data == 10..13  -> kria_service_note_off(track)
//   data == 20..23  -> kria_service_repeat(track)
void kria_clock_tick(void);
void kria_service_note_off(uint8_t track);
void kria_service_repeat(uint8_t track);

// External-clock edge from handler_Trigger for KR_EXT_CLOCK_INPUT; `level` is
// the pin state. Returns true if Kria consumed the edge.
bool kria_external_clock(uint8_t level);

// Output ownership: true when a script write to CV/TR channel `ch` (0-3) must be
// suppressed (Kria playing, ch is an un-muted track, not our own write).
bool kria_suppresses_output(uint8_t ch);

// Grid ownership + surface (from grid.c).
bool kria_owns_grid(void);
void kria_grid_key(uint8_t x, uint8_t y, uint8_t z);
void kria_grid_render(void);

#endif
