#ifndef _GRID_LED_H_
#define _GRID_LED_H_

// Shared LED helpers for the grid apps (Kria, Meadowphysics, Earthsea).
//
// One source of truth for the Ansible dim/med/bright ramp and for the render
// "finalize" pass every app runs after filling the LED buffer. Kept as an
// inline header (no .c) so the pure src/*_grid.c surfaces and the module shell
// can all share it without a link dependency.

#include <stdint.h>

// Brightness ramp (Ansible L0/L1/L2). Apps may alias these to local names.
#define GRID_L0 4   // dim
#define GRID_L1 8   // medium
#define GRID_L2 12  // bright

// The monome 16x8 grid LED buffer (monomeLedBuffer) is 128 cells, index y*16+x.
#define GRID_LED_COUNT 128

// Finalize a filled LED buffer: clamp every cell to the 0..15 range, and on a
// non-varibright grid (vari == 0) force any lit cell to full so dim ramps don't
// vanish. Call once at the end of each render.
static inline void grid_led_finalize(uint8_t* led, uint8_t vari) {
    for (uint16_t i = 0; i < GRID_LED_COUNT; i++) {
        if (led[i] > 15) led[i] = 15;
        if (!vari && led[i]) led[i] = 15;
    }
}

#endif
