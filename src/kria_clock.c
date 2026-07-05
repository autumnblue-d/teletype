// Kria clock -- see kria_clock.h. (Tempo/phase arbitration lives in
// grid_clock.c; only the deferred real-tick scaling is Kria-specific.)

#include "kria_clock.h"

uint16_t kria_clock_scale_duration(uint16_t dur_unscaled, uint32_t clock_delta,
                                   uint8_t tmul_tr) {
    // Ansible: dur = (u16)(unscaled * clock_scale),
    //          clock_scale = (clock_deltas * tmul[mTr]) / 384.0
    float clock_scale = ((float)clock_delta * (float)tmul_tr) / 384.0f;
    return (uint16_t)((float)dur_unscaled * clock_scale);
}

uint32_t kria_clock_repeat_ticks(uint32_t clock_delta, uint8_t tmul_tr,
                                 uint8_t rpt) {
    if (rpt == 0) rpt = 1;
    return (clock_delta * (uint32_t)tmul_tr) / (uint32_t)rpt;
}
