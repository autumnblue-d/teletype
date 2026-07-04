#ifndef _GRIDS_DATA_H_
#define _GRIDS_DATA_H_

#include <stdint.h>

// Mutable Instruments Grids topographic drum map.
// 3 instruments (0=BD, 1=SD, 2=HH), 32 steps per pattern.
#define GRIDS_INSTRUMENTS 3
#define GRIDS_STEPS 32
#define GRIDS_NODE_SIZE (GRIDS_INSTRUMENTS * GRIDS_STEPS)  // 96

// 5x5 grid of pointers into the 25 node patterns; indexed grids_map[i][j]
// where i = x >> 6, j = y >> 6 (0..4). Interpolation reads i/i+1, j/j+1.
extern const uint8_t *const grids_map[5][5];

#endif
