#ifndef _ES_GRID_H_
#define _ES_GRID_H_

// Earthsea grid surface: key handling + LED rendering, ported from Ansible's
// handler_ESGridKey + refresh_es (ansible_grid.c). Hardware-abstract and
// testable: operates on the engine (es_engine_t) plus a small grid
// interaction state, writing into a raw 16x8 LED buffer (index y*16+x). The
// mode shell routes the monome grid here when Earthsea owns it and blits
// into monomeLedBuffer.
//
// Scope vs Ansible: 16x8 primary view only. The 256-grid extras (bottom-half
// rune/edge/voice views, column-0 pattern hotkeys rows 8-15) and the Ansible
// preset screen are deliberately dropped (see EARTHSEA_PORT_PLAN.md §1).
//
// Layout (column 0 = control strip, top to bottom):
//   y0 start/stop   y1 pattern view   y2 arm/record   y3 loop toggle
//   y4 arp toggle   y5 edge view (hold)   y6 runes (hold)   y7 voices (hold)
// Playable field x1..15; key (15,0) = rest; row 0 scrubs while playing.
//
// Transport coupling: several presses (re)start playback. Key handling
// returns the engine's next-event interval when that happens so the shell
// can arm its one-shot play timer; the shell should also cancel the timer
// whenever rt.mode leaves es_playing after a key.

#include <stdint.h>

#include "es_engine.h"

// Main-view state (Ansible es_view_t).
#define ES_VIEW_MAIN 0
#define ES_VIEW_PATTERNS_HELD 1  // momentary, while y1 is held
#define ES_VIEW_PATTERNS 2       // locked (y1 then y0)

typedef struct {
    uint8_t view;        // ES_VIEW_*
    uint8_t runes_held;  // momentary overlays (control keys held)
    uint8_t edge_held;
    uint8_t voices_held;
    uint8_t ignore_arm_release;
    uint8_t blinker;  // toggled by the shell's ~288 ms timer (rec blink)
    uint8_t held[ES_KEYMAP_SIZE / 8];  // held-key bitmap, index y*16+x

    // caller-owned shared scale bank (16 x 8 interval steps) for the scale
    // display overlay; may be NULL (overlay then renders nothing).
    uint8_t (*scale_bank)[8];
} es_grid_state_t;

void es_grid_state_init(es_grid_state_t* g);

// Handle a grid key at (x,y), z = press(1)/release(0). Mutates engine cfg/rt
// and grid state. Returns the interval (ticks) to the next playback event
// when this key (re)started internal-clock playback, else 0.
uint32_t es_grid_process_key(es_engine_t* e, es_grid_state_t* g, uint8_t x,
                             uint8_t y, uint8_t z, uint32_t now);

// Render the current view into `led` (16*8 = 128 bytes, index y*16+x).
// Clears first, clamps to 15. If `vari` is 0 (non-varibright), any lit cell
// is forced to full brightness. `now` drives the playback position bar.
void es_grid_refresh(es_engine_t* e, es_grid_state_t* g, uint8_t* led,
                     uint8_t vari, uint32_t now);

#endif
