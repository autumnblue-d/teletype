#ifndef _MEADOWPHYSICS_GRID_H_
#define _MEADOWPHYSICS_GRID_H_

// Meadowphysics grid surface: key handling + LED rendering, ported from
// Ansible's handler_MPGridKey (NORMAL branch) and refresh_mp. Hardware-abstract
// and host-testable: operates on the engine plus a small grid-interaction state
// and a raw LED buffer. The module glue (grid.c) routes the monome grid here
// whenever MP mode is active, and blits into monomeLedBuffer.
//
// Layout (16 cols x 8 rows): the 8 rows are the 8 counters; the 16 columns are
// the range axis. Three edit sub-modes (positions / speed+trigger / rules) are
// switched on the grid by holding column 0 (=> speed) / column 1 (=> rules),
// exactly as Ansible does. OLED-level views (clock/config) are keyboard-driven
// (decision #6) and are not part of this surface.

#include <stdint.h>

#include "meadowphysics_engine.h"

// Grid sub-mode (within the positions surface).
#define MP_GRID_POSITIONS 0
#define MP_GRID_SPEED 1
#define MP_GRID_RULES 2

// Grid interaction state -- ephemeral, not persisted.
typedef struct {
    uint8_t edit_mode;       // MP_GRID_*
    uint8_t edit_row;        // row selected for speed/trigger/rules editing
    int8_t kcount;           // column-0 hold count (drives sub-mode switching)
    int8_t scount[MP_ROWS];  // per-row multi-press counter (range setting)
} mp_grid_state_t;

void mp_grid_state_init(mp_grid_state_t* g);

// Handle a grid key at (x, y), z = press(1)/release(0). Mutates engine cfg/rt
// and grid state per the current sub-mode.
void mp_grid_process_key(mp_engine_t* e, mp_grid_state_t* g, uint8_t x,
                         uint8_t y, uint8_t z);

// Render the current surface into `led` (16*8 = 128 bytes, index y*16 + x).
// Clears the buffer first. If `vari` is 0 (non-varibright grid) any lit cell is
// forced to full brightness so dim levels don't vanish (B5 fallback).
void mp_grid_refresh(mp_engine_t* e, mp_grid_state_t* g, uint8_t* led,
                     uint8_t vari);

#endif
