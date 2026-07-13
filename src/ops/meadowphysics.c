#include "meadowphysics.h"

#include "helpers.h"      // NOTUSED
#include "teletype_io.h"  // meadowphysics_op_reset / _stop

// Retargeted from external-Ansible i2c to the native Meadowphysics engine (A5).
// Channel arg: 0 = all rows, 1-8 = a single row.

// MP.PRESET -- get = current preset slot; set = load preset slot (0-7).
static void op_MP_PRESET_get(const void* NOTUSED(data),
                             scene_state_t* NOTUSED(ss),
                             exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_preset_get());
}
static void op_MP_PRESET_set(const void* NOTUSED(data),
                             scene_state_t* NOTUSED(ss),
                             exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_preset_set(cs_pop(cs));
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

// MP.SYNC -- clock source: 0 = internal, 1 = ext (Tr), 2 = Teletype metro (M).
static void op_MP_SYNC_get(const void* NOTUSED(data),
                           scene_state_t* NOTUSED(ss),
                           exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_sync_get());
}
static void op_MP_SYNC_set(const void* NOTUSED(data),
                           scene_state_t* NOTUSED(ss),
                           exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_sync_set(cs_pop(cs));
}

// MP.CLK -- manually advance the engine one full step (a script as the clock).
static void op_MP_CLK_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es),
                          command_state_t* NOTUSED(cs)) {
    meadowphysics_op_clock();
}

// MP.VOICE -- output mode: 0-4 = 1V / 2V / 4V / 8T / SCRIPT.
static void op_MP_VOICE_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_voice_get());
}
static void op_MP_VOICE_set(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_voice_set(cs_pop(cs));
}

// MP.PERIOD -- internal clock edge interval in ms.
static void op_MP_PERIOD_get(const void* NOTUSED(data),
                             scene_state_t* NOTUSED(ss),
                             exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_period_get());
}
static void op_MP_PERIOD_set(const void* NOTUSED(data),
                             scene_state_t* NOTUSED(ss),
                             exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_period_set(cs_pop(cs));
}

// MP.SCALE -- active scale-bank slot (0-15).
static void op_MP_SCALE_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_scale_get());
}
static void op_MP_SCALE_set(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    meadowphysics_op_scale_set(cs_pop(cs));
}

// MP.SCL -- scale semitone ladder rung, addressed by slot (0-15) + degree
// (0-7). Value clamped 0-7. Degree 0 = base; 1-7 = semitone deltas.
static void op_MP_SCL_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    // args pop first-written-first (like PN): slot, then degree.
    int16_t slot = cs_pop(cs);
    int16_t degree = cs_pop(cs);
    cs_push(cs, meadowphysics_op_ladder_get(slot, degree));
}
static void op_MP_SCL_set(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    int16_t slot = cs_pop(cs);
    int16_t degree = cs_pop(cs);
    int16_t val = cs_pop(cs);
    meadowphysics_op_ladder_set(slot, degree, val);
}

// MP.CFG row field [val] -- generic indexed cascade config accessor over the
// 8 rows (0 count, 1 speed, 2 min, 3 max, 4 rule, 5 rule-dest, 6 trigger mask,
// 7 toggle mask, 8 reset mask, 9 rule target, 10 smin, 11 smax). Mirrors KR.MP.
static void op_MP_CFG_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    int16_t field = cs_pop(cs);
    int16_t row = cs_pop(cs);
    cs_push(cs, meadowphysics_op_cfg(row, field, 0, 0));
}
static void op_MP_CFG_set(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    int16_t val = cs_pop(cs);
    int16_t field = cs_pop(cs);
    int16_t row = cs_pop(cs);
    meadowphysics_op_cfg(row, field, 1, val);
}

// MP.CV row -- the CV a row (1-8) plays; note_to_cv of its scale note.
static void op_MP_CV_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                         exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_push(cs, meadowphysics_op_cv(cs_pop(cs)));
}

const tele_op_t op_MP_PRESET =
    MAKE_GET_SET_OP(MP.PRESET, op_MP_PRESET_get, op_MP_PRESET_set, 0, true);
const tele_op_t op_MP_CFG =
    MAKE_GET_SET_OP(MP.CFG, op_MP_CFG_get, op_MP_CFG_set, 2, true);
const tele_op_t op_MP_CV = MAKE_GET_OP(MP.CV, op_MP_CV_get, 1, true);
const tele_op_t op_MP_RESET = MAKE_GET_OP(MP.RESET, op_MP_RESET_get, 1, false);
const tele_op_t op_MP_STOP = MAKE_GET_OP(MP.STOP, op_MP_STOP_get, 1, false);
const tele_op_t op_MP_RUN = MAKE_GET_OP(MP.RUN, op_MP_RUN_get, 1, false);
const tele_op_t op_MP_SYNC =
    MAKE_GET_SET_OP(MP.SYNC, op_MP_SYNC_get, op_MP_SYNC_set, 0, true);
const tele_op_t op_MP_CLK = MAKE_GET_OP(MP.CLK, op_MP_CLK_get, 0, false);
const tele_op_t op_MP_VOICE =
    MAKE_GET_SET_OP(MP.VOICE, op_MP_VOICE_get, op_MP_VOICE_set, 0, true);
const tele_op_t op_MP_PERIOD =
    MAKE_GET_SET_OP(MP.PERIOD, op_MP_PERIOD_get, op_MP_PERIOD_set, 0, true);
const tele_op_t op_MP_SCALE =
    MAKE_GET_SET_OP(MP.SCALE, op_MP_SCALE_get, op_MP_SCALE_set, 0, true);
const tele_op_t op_MP_SCL =
    MAKE_GET_SET_OP(MP.SCL, op_MP_SCL_get, op_MP_SCL_set, 2, true);
