#include "ops/midi.h"

#include "helpers.h"
#include "table.h"
#include "teletype_io.h"

static void op_MI_SYM_DOLLAR_get(const void *data, scene_state_t *ss,
                                 exec_state_t *es, command_state_t *cs);
static void op_MI_SYM_DOLLAR_set(const void *data, scene_state_t *ss,
                                 exec_state_t *es, command_state_t *cs);
static void op_MI_LE_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_LN_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_LNV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_LV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_LVV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_LO_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_LC_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_LCC_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_LCCV_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MI_NL_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_N_get(const void *data, scene_state_t *ss, exec_state_t *es,
                        command_state_t *cs);
static void op_MI_NV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_V_get(const void *data, scene_state_t *ss, exec_state_t *es,
                        command_state_t *cs);
static void op_MI_VV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_OL_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_O_get(const void *data, scene_state_t *ss, exec_state_t *es,
                        command_state_t *cs);
static void op_MI_CL_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_C_get(const void *data, scene_state_t *ss, exec_state_t *es,
                        command_state_t *cs);
static void op_MI_CC_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_CCV_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_LCH_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_NCH_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_OCH_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_CCH_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MI_CLKD_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MI_CLKD_set(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MI_CLKR_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);

// MIDI Out (MO.*) — send to a connected USB MIDI device. Default channel and
// output port (USB virtual cable: 0=A, 1=B) are module-level statics, mirroring
// EX.M.CH.
static u8 midi_out_channel = 0;
static u8 midi_out_port = 0;

static void op_MO_CH_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_PORT_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MO_PORT_set(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MI_LP_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_NP_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_OP_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MI_CP_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_CH_set(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_N_get(const void *data, scene_state_t *ss, exec_state_t *es,
                        command_state_t *cs);
static void op_MO_N_POUND_get(const void *data, scene_state_t *ss,
                              exec_state_t *es, command_state_t *cs);
static void op_MO_NO_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_NO_POUND_get(const void *data, scene_state_t *ss,
                               exec_state_t *es, command_state_t *cs);
static void op_MO_CC_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_CC_POUND_get(const void *data, scene_state_t *ss,
                               exec_state_t *es, command_state_t *cs);
static void op_MO_PB_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_PRG_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MO_CLK_get(const void *data, scene_state_t *ss, exec_state_t *es,
                          command_state_t *cs);
static void op_MO_START_get(const void *data, scene_state_t *ss,
                            exec_state_t *es, command_state_t *cs);
static void op_MO_STOP_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MO_CONT_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);
static void op_MO_NG_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_NG_POUND_get(const void *data, scene_state_t *ss,
                               exec_state_t *es, command_state_t *cs);
static void op_MO_TR_get(const void *data, scene_state_t *ss, exec_state_t *es,
                         command_state_t *cs);
static void op_MO_TR_POUND_get(const void *data, scene_state_t *ss,
                               exec_state_t *es, command_state_t *cs);
static void op_MO_NALL_get(const void *data, scene_state_t *ss,
                           exec_state_t *es, command_state_t *cs);

// Default gate for the momentary trigger ops (MO.TR / MO.TR#), in ms.
#define MO_TRIGGER_MS 10

// clang-format off

const tele_op_t op_MI_SYM_DOLLAR = MAKE_GET_SET_OP(MI.$, op_MI_SYM_DOLLAR_get, op_MI_SYM_DOLLAR_set, 1, true);
const tele_op_t op_MI_LE   = MAKE_GET_OP(MI.LE,   op_MI_LE_get,   0, true);
const tele_op_t op_MI_LN   = MAKE_GET_OP(MI.LN,   op_MI_LN_get,   0, true);
const tele_op_t op_MI_LNV  = MAKE_GET_OP(MI.LNV,  op_MI_LNV_get,  0, true);
const tele_op_t op_MI_LV   = MAKE_GET_OP(MI.LV,   op_MI_LV_get,   0, true);
const tele_op_t op_MI_LVV  = MAKE_GET_OP(MI.LVV,  op_MI_LVV_get,  0, true);
const tele_op_t op_MI_LO   = MAKE_GET_OP(MI.LO,   op_MI_LO_get,   0, true);
const tele_op_t op_MI_LC   = MAKE_GET_OP(MI.LC,   op_MI_LC_get,   0, true);
const tele_op_t op_MI_LCC  = MAKE_GET_OP(MI.LCC,  op_MI_LCC_get,  0, true);
const tele_op_t op_MI_LCCV = MAKE_GET_OP(MI.LCCV, op_MI_LCCV_get, 0, true);
const tele_op_t op_MI_NL   = MAKE_GET_OP(MI.NL,   op_MI_NL_get,   0, true);
const tele_op_t op_MI_N    = MAKE_GET_OP(MI.N,    op_MI_N_get,    0, true);
const tele_op_t op_MI_NV   = MAKE_GET_OP(MI.NV,   op_MI_NV_get,   0, true);
const tele_op_t op_MI_V    = MAKE_GET_OP(MI.V,    op_MI_V_get,    0, true);
const tele_op_t op_MI_VV   = MAKE_GET_OP(MI.VV,   op_MI_VV_get,   0, true);
const tele_op_t op_MI_OL   = MAKE_GET_OP(MI.OL,   op_MI_OL_get,   0, true);
const tele_op_t op_MI_O    = MAKE_GET_OP(MI.O,    op_MI_O_get,    0, true);
const tele_op_t op_MI_CL   = MAKE_GET_OP(MI.CL,   op_MI_CL_get,   0, true);
const tele_op_t op_MI_C    = MAKE_GET_OP(MI.C,    op_MI_C_get,    0, true);
const tele_op_t op_MI_CC   = MAKE_GET_OP(MI.CC,   op_MI_CC_get,   0, true);
const tele_op_t op_MI_CCV  = MAKE_GET_OP(MI.CCV,  op_MI_CCV_get,  0, true);
const tele_op_t op_MI_LCH  = MAKE_GET_OP(MI.LCH,  op_MI_LCH_get,  0, true);
const tele_op_t op_MI_NCH  = MAKE_GET_OP(MI.NCH,  op_MI_NCH_get,  0, true);
const tele_op_t op_MI_OCH  = MAKE_GET_OP(MI.OCH,  op_MI_OCH_get,  0, true);
const tele_op_t op_MI_CCH  = MAKE_GET_OP(MI.CCH,  op_MI_CCH_get,  0, true);
const tele_op_t op_MI_LP   = MAKE_GET_OP(MI.LP,   op_MI_LP_get,   0, true);
const tele_op_t op_MI_NP   = MAKE_GET_OP(MI.NP,   op_MI_NP_get,   0, true);
const tele_op_t op_MI_OP   = MAKE_GET_OP(MI.OP,   op_MI_OP_get,   0, true);
const tele_op_t op_MI_CP   = MAKE_GET_OP(MI.CP,   op_MI_CP_get,   0, true);
const tele_op_t op_MI_CLKR = MAKE_GET_OP(MI.CLKR, op_MI_CLKR_get, 0, false);
const tele_op_t op_MI_CLKD = MAKE_GET_SET_OP(MI.CLKD, op_MI_CLKD_get, op_MI_CLKD_set, 0, true);

const tele_op_t op_MO_CH       = MAKE_GET_SET_OP(MO.CH, op_MO_CH_get, op_MO_CH_set, 0, true);
const tele_op_t op_MO_PORT     = MAKE_GET_SET_OP(MO.PORT, op_MO_PORT_get, op_MO_PORT_set, 0, true);
const tele_op_t op_MO_N        = MAKE_GET_OP(MO.N,      op_MO_N_get,        2, false);
const tele_op_t op_MO_N_POUND  = MAKE_GET_OP(MO.N#,     op_MO_N_POUND_get,  3, false);
const tele_op_t op_MO_NO       = MAKE_GET_OP(MO.NO,     op_MO_NO_get,       1, false);
const tele_op_t op_MO_NO_POUND = MAKE_GET_OP(MO.NO#,    op_MO_NO_POUND_get, 2, false);
const tele_op_t op_MO_CC       = MAKE_GET_OP(MO.CC,     op_MO_CC_get,       2, false);
const tele_op_t op_MO_CC_POUND = MAKE_GET_OP(MO.CC#,    op_MO_CC_POUND_get, 3, false);
const tele_op_t op_MO_PB       = MAKE_GET_OP(MO.PB,     op_MO_PB_get,       1, false);
const tele_op_t op_MO_PRG      = MAKE_GET_OP(MO.PRG,    op_MO_PRG_get,      1, false);
const tele_op_t op_MO_CLK      = MAKE_GET_OP(MO.CLK,    op_MO_CLK_get,      0, false);
const tele_op_t op_MO_START    = MAKE_GET_OP(MO.START,  op_MO_START_get,    0, false);
const tele_op_t op_MO_STOP     = MAKE_GET_OP(MO.STOP,   op_MO_STOP_get,     0, false);
const tele_op_t op_MO_CONT     = MAKE_GET_OP(MO.CONT,   op_MO_CONT_get,     0, false);
const tele_op_t op_MO_NG       = MAKE_GET_OP(MO.NG,     op_MO_NG_get,       3, false);
const tele_op_t op_MO_NG_POUND = MAKE_GET_OP(MO.NG#,    op_MO_NG_POUND_get, 4, false);
const tele_op_t op_MO_TR       = MAKE_GET_OP(MO.TR,     op_MO_TR_get,       2, false);
const tele_op_t op_MO_TR_POUND = MAKE_GET_OP(MO.TR#,    op_MO_TR_POUND_get, 3, false);
const tele_op_t op_MO_NALL     = MAKE_GET_OP(MO.NALL,   op_MO_NALL_get,     0, false);

// clang-format on

static void op_MI_SYM_DOLLAR_get(const void *NOTUSED(data), scene_state_t *ss,
                                 exec_state_t *NOTUSED(es),
                                 command_state_t *cs) {
    uint16_t event = cs_pop(cs);
    int16_t script = -1;

    switch (event) {
        case 0:
            script = ss->midi.on_script;
            if (script != ss->midi.off_script || script != ss->midi.cc_script ||
                script != ss->midi.clk_script ||
                script != ss->midi.start_script ||
                script != ss->midi.stop_script ||
                script != ss->midi.continue_script)
                script = -1;
            break;
        case 1: script = ss->midi.on_script; break;
        case 2: script = ss->midi.off_script; break;
        case 3: script = ss->midi.cc_script; break;
        case 4: script = ss->midi.clk_script; break;
        case 5: script = ss->midi.start_script; break;
        case 6: script = ss->midi.stop_script; break;
        case 7: script = ss->midi.continue_script; break;
        default: break;
    }

    cs_push(cs, script == -1 ? script : script + 1);
}

static void op_MI_SYM_DOLLAR_set(const void *NOTUSED(data), scene_state_t *ss,
                                 exec_state_t *NOTUSED(es),
                                 command_state_t *cs) {
    uint16_t event = cs_pop(cs);
    s16 script = cs_pop(cs) - 1;
    if (script < 0 || script > INIT_SCRIPT) script = -1;

    switch (event) {
        case 0:
            ss->midi.on_script = script;
            ss->midi.off_script = script;
            ss->midi.cc_script = script;
            ss->midi.clk_script = script;
            ss->midi.start_script = script;
            ss->midi.stop_script = script;
            ss->midi.continue_script = script;
            ss->midi.on_count = 0;
            ss->midi.off_count = 0;
            ss->midi.cc_count = 0;
            break;
        case 1:
            ss->midi.on_script = script;
            ss->midi.on_count = 0;
            break;
        case 2:
            ss->midi.off_script = script;
            ss->midi.off_count = 0;
            break;
        case 3:
            ss->midi.cc_script = script;
            ss->midi.cc_count = 0;
            break;
        case 4: ss->midi.clk_script = script; break;
        case 5: ss->midi.start_script = script; break;
        case 6: ss->midi.stop_script = script; break;
        case 7: ss->midi.continue_script = script; break;
        default: break;
    }
}

static void op_MI_LE_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_event_type);
}

static void op_MI_LN_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_note);
}

static void op_MI_LNV_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, table_n[ss->midi.last_note]);
}

static void op_MI_LV_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_velocity);
}

static void op_MI_LVV_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_velocity * 129);
}

static void op_MI_LO_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_note);
}

static void op_MI_LC_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_controller);
}

static void op_MI_LCC_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_cc);
}

static void op_MI_LCCV_get(const void *NOTUSED(data), scene_state_t *ss,
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_cc * 129);
}

static void op_MI_NL_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.on_count);
}

static void op_MI_N_get(const void *NOTUSED(data), scene_state_t *ss,
                        exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count ? 0 : ss->midi.note_on[i - 1]);
}

static void op_MI_NV_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count
                    ? 0
                    : table_n[ss->midi.note_on[i - 1]]);
}

static void op_MI_V_get(const void *NOTUSED(data), scene_state_t *ss,
                        exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count ? 0 : ss->midi.note_vel[i - 1]);
}

static void op_MI_VV_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count
                    ? 0
                    : ss->midi.note_vel[i - 1] * 129);
}

static void op_MI_OL_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.off_count);
}

static void op_MI_O_get(const void *NOTUSED(data), scene_state_t *ss,
                        exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.off_count ? 0 : ss->midi.note_off[i - 1]);
}

static void op_MI_CL_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.cc_count);
}

static void op_MI_C_get(const void *NOTUSED(data), scene_state_t *ss,
                        exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.cc_count ? 0 : ss->midi.cn[i - 1]);
}

static void op_MI_CC_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.cc_count ? 0 : ss->midi.cc[i - 1]);
}

static void op_MI_CCV_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.cc_count ? 0 : ss->midi.cc[i - 1] * 129);
}

static void op_MI_LCH_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_channel + 1);
}

static void op_MI_NCH_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count
                    ? 0
                    : ss->midi.on_channel[i - 1] + 1);
}

static void op_MI_OCH_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.off_count
                    ? 0
                    : ss->midi.off_channel[i - 1] + 1);
}

static void op_MI_CCH_get(const void *NOTUSED(data), scene_state_t *ss,
                          exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.cc_count
                    ? 0
                    : ss->midi.cc_channel[i - 1] + 1);
}

// Port (USB virtual cable, 0-based: 0=A, 1=B) the event arrived on.
static void op_MI_LP_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.last_port);
}

static void op_MI_NP_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.on_count ? 0 : ss->midi.on_port[i - 1]);
}

static void op_MI_OP_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.off_count ? 0 : ss->midi.off_port[i - 1]);
}

static void op_MI_CP_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *es, command_state_t *cs) {
    s16 i = es_variables(es)->i;
    cs_push(cs, i < 1 || i > ss->midi.cc_count ? 0 : ss->midi.cc_port[i - 1]);
}

static void op_MI_CLKD_get(const void *NOTUSED(data), scene_state_t *ss,
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, ss->midi.clock_div);
}

static void op_MI_CLKD_set(const void *NOTUSED(data), scene_state_t *ss,
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 clock_div = cs_pop(cs);
    if (clock_div < 1 || clock_div > 24) return;
    ss->midi.clock_div = clock_div;
    reset_midi_counter();
}

static void op_MI_CLKR_get(const void *NOTUSED(data), scene_state_t *ss,
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    reset_midi_counter();
}

// Build a raw MIDI message and hand it to the module's USB MIDI out hook.
// midi_write_packet() derives the USB CIN from status >> 4, so unused trailing
// bytes (0) are correct for 2-byte (PRG) and 1-byte (realtime) messages.
static void mo_send(u8 status, u8 d1, u8 d2) {
    u8 pack[3] = { status, d1, d2 };
    tele_midi_out(midi_out_port, pack, 3);
}

// Send a Note-Off on an explicit port/channel (used by the scheduled-off pool,
// whose entries each remember the port their Note-On went out on).
static void mo_send_off(u8 port, u8 channel, u8 note) {
    u8 pack[3] = { 0x80 + channel, note, 0 };
    tele_midi_out(port, pack, 3);
}

// Register a Note-Off to fire after dur ms. If this (port,channel,note) is
// already held, refresh its deadline (retrigger stays a single Note-Off); else
// take a free slot; if the pool is full, steal the slot nearest to firing by
// sending its Note-Off early so no held note is ever leaked.
static void mo_schedule_off(scene_state_t *ss, u8 port, u8 channel, u8 note,
                            s16 dur) {
    scene_midi_out_t *m = &ss->midi_out;
    for (u8 i = 0; i < MIDI_OUT_NOTE_SLOTS; i++) {
        if (m->notes[i].active && m->notes[i].port == port &&
            m->notes[i].channel == channel && m->notes[i].note == note) {
            m->notes[i].ticks_remaining = dur;
            return;
        }
    }
    for (u8 i = 0; i < MIDI_OUT_NOTE_SLOTS; i++) {
        if (!m->notes[i].active) {
            m->notes[i].active = 1;
            m->notes[i].port = port;
            m->notes[i].channel = channel;
            m->notes[i].note = note;
            m->notes[i].ticks_remaining = dur;
            m->count++;
            return;
        }
    }
    u8 victim = 0;
    for (u8 i = 1; i < MIDI_OUT_NOTE_SLOTS; i++) {
        if (m->notes[i].ticks_remaining < m->notes[victim].ticks_remaining)
            victim = i;
    }
    mo_send_off(m->notes[victim].port, m->notes[victim].channel,
                m->notes[victim].note);
    m->notes[victim].port = port;
    m->notes[victim].channel = channel;
    m->notes[victim].note = note;
    m->notes[victim].ticks_remaining = dur;
}

// Send a Note-On now and schedule its Note-Off dur ms later.
static void mo_note_on_dur(scene_state_t *ss, u8 channel, u16 note,
                           u16 velocity, s16 dur) {
    if (note > 127) return;
    if (velocity > 127) velocity = 127;
    mo_send(0x90 + channel, note, velocity);
    mo_schedule_off(ss, midi_out_port, channel, note, dur);
}

// Tick service: count down every held note and emit its Note-Off when due.
// Called from tele_tick() with the ms elapsed since the last tick.
void mo_process_note_offs(scene_state_t *ss, u8 time) {
    scene_midi_out_t *m = &ss->midi_out;
    if (m->count == 0) return;
    for (u8 i = 0; i < MIDI_OUT_NOTE_SLOTS; i++) {
        if (!m->notes[i].active) continue;
        m->notes[i].ticks_remaining -= time;
        if (m->notes[i].ticks_remaining <= 0) {
            mo_send_off(m->notes[i].port, m->notes[i].channel,
                        m->notes[i].note);
            m->notes[i].active = 0;
            m->count--;
        }
    }
}

// Release every held note immediately (MO.NALL, and on scene load).
void mo_flush_note_offs(scene_state_t *ss) {
    scene_midi_out_t *m = &ss->midi_out;
    for (u8 i = 0; i < MIDI_OUT_NOTE_SLOTS; i++) {
        if (!m->notes[i].active) continue;
        mo_send_off(m->notes[i].port, m->notes[i].channel, m->notes[i].note);
        m->notes[i].active = 0;
    }
    m->count = 0;
}

static void op_MO_CH_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, midi_out_channel + 1);
}

static void op_MO_CH_set(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    if (ch < 0 || ch > 15) return;
    midi_out_channel = ch;
}

static void op_MO_PORT_get(const void *NOTUSED(data),
                           scene_state_t *NOTUSED(ss),
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    cs_push(cs, midi_out_port);
}

static void op_MO_PORT_set(const void *NOTUSED(data),
                           scene_state_t *NOTUSED(ss),
                           exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 port = cs_pop(cs);
    if (port < 0 || port > 15) return;
    midi_out_port = port;
}

static void op_MO_N_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                        exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    if (note > 127) return;
    if (velocity > 127) velocity = 127;
    mo_send(0x90 + midi_out_channel, note, velocity);
}

static void op_MO_N_POUND_get(const void *NOTUSED(data),
                              scene_state_t *NOTUSED(ss),
                              exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    if (ch < 0 || ch > 15) return;
    if (note > 127) return;
    if (velocity > 127) velocity = 127;
    mo_send(0x90 + ch, note, velocity);
}

static void op_MO_NO_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 note = cs_pop(cs);
    if (note > 127) return;
    mo_send(0x80 + midi_out_channel, note, 0);
}

static void op_MO_NO_POUND_get(const void *NOTUSED(data),
                               scene_state_t *NOTUSED(ss),
                               exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    u16 note = cs_pop(cs);
    if (ch < 0 || ch > 15) return;
    if (note > 127) return;
    mo_send(0x80 + ch, note, 0);
}

static void op_MO_CC_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 controller = cs_pop(cs);
    u16 value = cs_pop(cs);
    if (controller > 127) return;
    if (value > 127) value = 127;
    mo_send(0xB0 + midi_out_channel, controller, value);
}

static void op_MO_CC_POUND_get(const void *NOTUSED(data),
                               scene_state_t *NOTUSED(ss),
                               exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    u16 controller = cs_pop(cs);
    u16 value = cs_pop(cs);
    if (ch < 0 || ch > 15) return;
    if (controller > 127) return;
    if (value > 127) value = 127;
    mo_send(0xB0 + ch, controller, value);
}

static void op_MO_PB_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 bend = cs_pop(cs);
    if (bend > 16383) bend = 16383;
    // 14-bit value split into two 7-bit data bytes: LSB first, then MSB.
    mo_send(0xE0 + midi_out_channel, bend & 0x7F, (bend >> 7) & 0x7F);
}

static void op_MO_PRG_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                          exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 program = cs_pop(cs);
    if (program > 127) return;
    mo_send(0xC0 + midi_out_channel, program, 0);
}

static void op_MO_CLK_get(const void *NOTUSED(data), scene_state_t *NOTUSED(ss),
                          exec_state_t *NOTUSED(es),
                          command_state_t *NOTUSED(cs)) {
    mo_send(0xF8, 0, 0);
}

static void op_MO_START_get(const void *NOTUSED(data),
                            scene_state_t *NOTUSED(ss),
                            exec_state_t *NOTUSED(es),
                            command_state_t *NOTUSED(cs)) {
    mo_send(0xFA, 0, 0);
}

static void op_MO_STOP_get(const void *NOTUSED(data),
                           scene_state_t *NOTUSED(ss),
                           exec_state_t *NOTUSED(es),
                           command_state_t *NOTUSED(cs)) {
    mo_send(0xFC, 0, 0);
}

static void op_MO_CONT_get(const void *NOTUSED(data),
                           scene_state_t *NOTUSED(ss),
                           exec_state_t *NOTUSED(es),
                           command_state_t *NOTUSED(cs)) {
    mo_send(0xFB, 0, 0);
}

static void op_MO_NG_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    s16 dur = cs_pop(cs);
    if (dur < 1) dur = 1;
    mo_note_on_dur(ss, midi_out_channel, note, velocity, dur);
}

static void op_MO_NG_POUND_get(const void *NOTUSED(data), scene_state_t *ss,
                               exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    s16 dur = cs_pop(cs);
    if (ch < 0 || ch > 15) return;
    if (dur < 1) dur = 1;
    mo_note_on_dur(ss, ch, note, velocity, dur);
}

static void op_MO_TR_get(const void *NOTUSED(data), scene_state_t *ss,
                         exec_state_t *NOTUSED(es), command_state_t *cs) {
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    mo_note_on_dur(ss, midi_out_channel, note, velocity, MO_TRIGGER_MS);
}

static void op_MO_TR_POUND_get(const void *NOTUSED(data), scene_state_t *ss,
                               exec_state_t *NOTUSED(es), command_state_t *cs) {
    s16 ch = cs_pop(cs) - 1;
    u16 note = cs_pop(cs);
    u16 velocity = cs_pop(cs);
    if (ch < 0 || ch > 15) return;
    mo_note_on_dur(ss, ch, note, velocity, MO_TRIGGER_MS);
}

static void op_MO_NALL_get(const void *NOTUSED(data), scene_state_t *ss,
                           exec_state_t *NOTUSED(es),
                           command_state_t *NOTUSED(cs)) {
    mo_flush_note_offs(ss);
}
