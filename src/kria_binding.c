// Kria output binding -- see kria_binding.h.

#include "kria_binding.h"

#include <stddef.h>  // NULL

#include "grid_binding.h"  // grid_bind_tr / grid_bind_cv (shared leaves)
#include "teletype_io.h"   // tele_cv_slew

static void bind_cv_slew(void* ctx, uint8_t ch, uint16_t slew) {
    (void)ctx;
    tele_cv_slew(ch, (int16_t)slew);
}

static const kria_output_t OUTPUT = {
    .tr = grid_bind_tr, .cv = grid_bind_cv, .cv_slew = bind_cv_slew, .ctx = NULL
};

const kria_output_t* kria_binding_output(void) {
    return &OUTPUT;
}
