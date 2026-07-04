#include "meadowphysics.h"

#include "helpers.h"      // NOTUSED
#include "teletype_io.h"  // meadowphysics_op_reset / _stop

// Retargeted from external-Ansible i2c to the native Meadowphysics engine (A5).
// Channel arg: 0 = all rows, 1-8 = a single row.

static void op_MP_PRESET_get(const void* NOTUSED(data),
                             scene_state_t* NOTUSED(ss),
                             exec_state_t* NOTUSED(es), command_state_t* cs) {
    // Native MP has no presets (state is per-scene, decision #3); accept and
    // ignore the argument so existing scripts don't error.
    cs_pop(cs);
}

static void op_MP_RESET_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_reset(cs_pop(cs));
}

static void op_MP_STOP_get(const void* NOTUSED(data),
                           scene_state_t* NOTUSED(ss),
                           exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_stop(cs_pop(cs));
}

static void op_MP_RUN_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_run(cs_pop(cs));
}

const tele_op_t op_MP_PRESET =
    MAKE_GET_OP(MP.PRESET, op_MP_PRESET_get, 1, false);
const tele_op_t op_MP_RESET = MAKE_GET_OP(MP.RESET, op_MP_RESET_get, 1, false);
const tele_op_t op_MP_STOP = MAKE_GET_OP(MP.STOP, op_MP_STOP_get, 1, false);
const tele_op_t op_MP_RUN = MAKE_GET_OP(MP.RUN, op_MP_RUN_get, 1, false);
