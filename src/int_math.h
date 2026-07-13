#ifndef _INT_MATH_H_
#define _INT_MATH_H_

// Small integer helpers shared by the grid apps (mirror Ansible min/max/
// sum_clip). Header-only static inline -- no linkage, no separate .o.

static inline int imin(int a, int b) {
    return a < b ? a : b;
}
static inline int imax(int a, int b) {
    return a > b ? a : b;
}
static inline int sum_clip(int l, int r, int clip) {
    return imin(clip, imax(0, l + r));
}

#endif
