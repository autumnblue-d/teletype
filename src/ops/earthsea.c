// ES.* ops. Historically these were i2c leader commands for an external
// Ansible running Earthsea; with the native Earthsea port they are retargeted
// at the on-board engine via the es_op_* seams (implemented in
// module/earthsea_mode.c; simulator/tests provide stubs), exactly like the
// KR.* / MP.* retarget. Exceptions kept as i2c passthrough:
//   ES.PRESET -- presets don't exist in the native port (single global bank);
//                still useful against a real external Ansible.
//   ES.TRIPLE -- targets the original Earthsea module (Ansible ignores it too).

#include "earthsea.h"

#include "helpers.h"
#include "ii.h"
#include "teletype_io.h"

static void op_ES_MODE_get(const void* data, scene_state_t* ss,
                           exec_state_t* es, command_state_t* cs);
static void op_ES_CLOCK_get(const void* data, scene_state_t* ss,
                            exec_state_t* es, command_state_t* cs);
static void op_ES_RESET_get(const void* data, scene_state_t* ss,
                            exec_state_t* es, command_state_t* cs);
static void op_ES_PATTERN_get(const void* data, scene_state_t* ss,
                              exec_state_t* es, command_state_t* cs);
static void op_ES_TRANS_get(const void* data, scene_state_t* ss,
                            exec_state_t* es, command_state_t* cs);
static void op_ES_STOP_get(const void* data, scene_state_t* ss,
                           exec_state_t* es, command_state_t* cs);
static void op_ES_MAGIC_get(const void* data, scene_state_t* ss,
                            exec_state_t* es, command_state_t* cs);
static void op_ES_CV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_ES_RUN_get(const void* data, scene_state_t* ss, exec_state_t* es,
                          command_state_t* cs);

// clang-format off
const tele_op_t op_ES_PRESET  = MAKE_SIMPLE_I2C_OP(ES.PRESET, ES_PRESET);
const tele_op_t op_ES_MODE    = MAKE_GET_OP(ES.MODE   , op_ES_MODE_get   , 1, false);
const tele_op_t op_ES_CLOCK   = MAKE_GET_OP(ES.CLOCK  , op_ES_CLOCK_get  , 1, false);
const tele_op_t op_ES_RESET   = MAKE_GET_OP(ES.RESET  , op_ES_RESET_get  , 1, false);
const tele_op_t op_ES_PATTERN = MAKE_GET_OP(ES.PATTERN, op_ES_PATTERN_get, 1, false);
const tele_op_t op_ES_TRANS   = MAKE_GET_OP(ES.TRANS  , op_ES_TRANS_get  , 1, false);
const tele_op_t op_ES_STOP    = MAKE_GET_OP(ES.STOP   , op_ES_STOP_get   , 1, false);
const tele_op_t op_ES_TRIPLE  = MAKE_SIMPLE_I2C_OP(ES.TRIPLE, ES_TRIPLE);
const tele_op_t op_ES_MAGIC   = MAKE_GET_OP(ES.MAGIC  , op_ES_MAGIC_get  , 1, false);
const tele_op_t op_ES_CV      = MAKE_GET_OP(ES.CV     , op_ES_CV_get     , 1, true);
const tele_op_t op_ES_RUN     = MAKE_GET_OP(ES.RUN    , op_ES_RUN_get    , 1, false);
// clang-format on

static void op_ES_MODE_get(const void* NOTUSED(data),
                           scene_state_t* NOTUSED(ss),
                           exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_mode(cs_pop(cs));
}

static void op_ES_CLOCK_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_clock(cs_pop(cs));
}

static void op_ES_RESET_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_reset(cs_pop(cs));
}

static void op_ES_PATTERN_get(const void* NOTUSED(data),
                              scene_state_t* NOTUSED(ss),
                              exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_pattern(cs_pop(cs));
}

static void op_ES_TRANS_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_trans(cs_pop(cs));
}

static void op_ES_STOP_get(const void* NOTUSED(data),
                           scene_state_t* NOTUSED(ss),
                           exec_state_t* NOTUSED(es), command_state_t* cs) {
    cs_pop(cs);  // arity preserved from the i2c op; value unused
    es_op_stop();
}

static void op_ES_MAGIC_get(const void* NOTUSED(data),
                            scene_state_t* NOTUSED(ss),
                            exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_magic(cs_pop(cs));
}

static void op_ES_CV_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    int16_t a = cs_pop(cs);
    cs_push(cs, es_op_cv(a - 1));  // 1-based voice, like the i2c op
}

static void op_ES_RUN_get(const void* NOTUSED(data), scene_state_t* NOTUSED(ss),
                          exec_state_t* NOTUSED(es), command_state_t* cs) {
    es_op_run(cs_pop(cs));
}
