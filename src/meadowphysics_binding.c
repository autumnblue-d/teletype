// Meadowphysics output binding -- see meadowphysics_binding.h.

#include "meadowphysics_binding.h"

#include <stddef.h>  // NULL

#include "grid_binding.h"  // grid_bind_tr / grid_bind_cv (shared leaves)
#include "teletype_io.h"   // tele_cv

static void bind_cv_gate(void* ctx, uint8_t ch, uint8_t on) {
    (void)ctx;
    tele_cv(ch, on ? MP_CV_FULL : 0, 0);
}

static const mp_output_t OUTPUT = {
    .tr = grid_bind_tr, .cv = grid_bind_cv, .cv_gate = bind_cv_gate, .ctx = NULL
};

const mp_output_t* mp_binding_output(void) {
    return &OUTPUT;
}
