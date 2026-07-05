#ifndef _MEADOWPHYSICS_MODE_H_
#define _MEADOWPHYSICS_MODE_H_

#include <stdbool.h>
#include <stdint.h>

// Trigger input (0-indexed) that carries the external clock when enabled (A3).
// 0 = the first trigger input. Revisit alongside external-clock validation.
#define MP_EXT_CLOCK_INPUT 0

// Native Meadowphysics mode shell: owns the engine + clock instances, drives
// the dedicated clock timer, binds output, and renders the OLED. See
// MEADOWPHYSICS_PORT_PLAN.md.

// Enter MP mode (called from set_mode). Inits the engine/clock on first use,
// claims exclusive output ownership, and starts the clock timer.
void set_meadowphysics_mode(void);

// Leave the MP view (called from set_mode when switching away). The engine
// keeps running in the background; this only relinquishes the keyboard/grid.
void meadowphysics_mode_exit(void);

// Play/pause the MP engine, independent of which view is front-most. Bound to
// Space in the MP view, and also driven by the MP.RUN script op. While running,
// MP owns the CV/TR outputs; stopping releases them back to scripts.
void meadowphysics_toggle_run(void);

// Keyboard handler (dispatched from process_keypress).
void process_meadowphysics_keys(uint8_t key, uint8_t mod_key, bool is_held_key);

// OLED refresh (dispatched from handler_ScreenRefresh); returns a dirty
// bitmask.
uint8_t screen_refresh_meadowphysics(void);

// handler_AppCustom event code: our clock ISR posts it, main.c dispatches it.
#define MP_APPEVT_CLOCK 1  // internal clock tick

// Internal clock tick, dispatched from handler_AppCustom (MP_APPEVT_CLOCK).
void meadowphysics_clock_tick(void);

// External-clock edge from handler_Trigger for the configured Tr input (A3);
// `level` is the input pin state. Returns true if MP consumed the edge (MP mode
// active and external clock enabled), so the caller can skip the script; false
// otherwise (input is handled normally).
bool meadowphysics_external_clock(uint8_t level);

// Output ownership: true when a script write to CV/TR channel `ch` (0-3) must
// be suppressed -- MP is playing and uses `ch` in the current voice mode.
// Channels MP doesn't use (e.g. CV2-4/TR2-4 in 1V) stay free for scripts.
bool meadowphysics_suppresses_output(uint8_t ch);

// True when MP should drive the monome grid: while its view is front-most, or
// while it's playing (so a running sequence keeps animating the grid even from
// another mode). grid.c routes grid key/refresh to MP when this is set.
bool meadowphysics_owns_grid(void);

// Grid surface (dispatched from grid.c when meadowphysics_owns_grid()).
void meadowphysics_grid_key(uint8_t x, uint8_t y, uint8_t z);
void meadowphysics_grid_render(void);  // fills monomeLedBuffer

// Native engine control from MP.* ops (teletype_io.h). channel 0 = all rows,
// 1-8 = a single row. No-op until MP has been entered at least once.
void meadowphysics_op_reset(int16_t channel);
void meadowphysics_op_stop(int16_t channel);

#endif
