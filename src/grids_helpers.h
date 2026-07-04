#ifndef _GRIDS_HELPERS_H_
#define _GRIDS_HELPERS_H_

// Pure, stateless port of Mutable Instruments Grids' topographic drum-map
// read (ReadDrumMap) and trigger/accent evaluation (EvaluateDrums core).
// No clock, swing, euclidean, or perturbation state - Teletype drives the
// step counter and supplies all parameters.
//
//   instrument : 0..2  (0=BD, 1=SD, 2=HH)
//   step       : 0..31 (negative wraps, like drum())
//   x, y       : 0..255 map position (clamped)
//   density    : 0..255 (clamped)

// Interpolated map level 0..255 for (instrument, step) at position (x, y).
int grids_level(int instrument, int x, int y, int step);

// 1 if the instrument fires at this step for the given density, else 0.
int grids_trigger(int instrument, int x, int y, int density, int step);

// 1 if the fired level is an accent (level > 192), else 0.
int grids_accent(int instrument, int x, int y, int density, int step);

#endif
