#ifndef _SCALE_UTIL_H_
#define _SCALE_UTIL_H_

#include <stdint.h>

// Cumulative scale (Ansible calc_scale): turn 8 per-degree interval deltas into
// 8 absolute semitone offsets. out[0]=intervals[0], out[i]=out[i-1]+intervals[i].
// Header-only static inline -- no linkage, no separate .o.
static inline void cumulative_scale(uint8_t out[8], const uint8_t intervals[8]) {
    out[0] = intervals[0];
    for (uint8_t i = 1; i < 8; i++)
        out[i] = (uint8_t)(out[i - 1] + intervals[i]);
}

#endif
