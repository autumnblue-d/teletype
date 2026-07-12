#ifndef _OPS_MIDI_H_
#define _OPS_MIDI_H_

#include "ops/op.h"

extern const tele_op_t op_MI_SYM_DOLLAR;
extern const tele_op_t op_MI_LE;
extern const tele_op_t op_MI_LN;
extern const tele_op_t op_MI_LNV;
extern const tele_op_t op_MI_LV;
extern const tele_op_t op_MI_LVV;
extern const tele_op_t op_MI_LO;
extern const tele_op_t op_MI_LC;
extern const tele_op_t op_MI_LCC;
extern const tele_op_t op_MI_LCCV;
extern const tele_op_t op_MI_NL;
extern const tele_op_t op_MI_N;
extern const tele_op_t op_MI_NV;
extern const tele_op_t op_MI_V;
extern const tele_op_t op_MI_VV;
extern const tele_op_t op_MI_OL;
extern const tele_op_t op_MI_O;
extern const tele_op_t op_MI_CL;
extern const tele_op_t op_MI_C;
extern const tele_op_t op_MI_CC;
extern const tele_op_t op_MI_CCV;
extern const tele_op_t op_MI_LCH;
extern const tele_op_t op_MI_NCH;
extern const tele_op_t op_MI_OCH;
extern const tele_op_t op_MI_CCH;
extern const tele_op_t op_MI_CLKD;
extern const tele_op_t op_MI_CLKR;
extern const tele_op_t op_MI_LP;
extern const tele_op_t op_MI_NP;
extern const tele_op_t op_MI_OP;
extern const tele_op_t op_MI_CP;

extern const tele_op_t op_MO_CH;
extern const tele_op_t op_MO_PORT;
extern const tele_op_t op_MO_N;
extern const tele_op_t op_MO_N_POUND;
extern const tele_op_t op_MO_NO;
extern const tele_op_t op_MO_NO_POUND;
extern const tele_op_t op_MO_CC;
extern const tele_op_t op_MO_CC_POUND;
extern const tele_op_t op_MO_PB;
extern const tele_op_t op_MO_PRG;
extern const tele_op_t op_MO_CLK;
extern const tele_op_t op_MO_START;
extern const tele_op_t op_MO_STOP;
extern const tele_op_t op_MO_CONT;
extern const tele_op_t op_MO_NG;
extern const tele_op_t op_MO_NG_POUND;
extern const tele_op_t op_MO_TR;
extern const tele_op_t op_MO_TR_POUND;
extern const tele_op_t op_MO_NALL;

// Service the MIDI-out scheduled-Note-Off pool. mo_process_note_offs() is
// called each tick (time = ms elapsed); mo_flush_note_offs() releases all held
// notes at once (MO.NALL and scene load).
void mo_process_note_offs(scene_state_t *ss, uint8_t time);
void mo_flush_note_offs(scene_state_t *ss);

#endif
