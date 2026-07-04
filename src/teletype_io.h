#ifndef _TELETYPE_IO_H_
#define _TELETYPE_IO_H_

#include <stdbool.h>
#include <stdint.h>

#define SUB_MODE_OFF 0
#define SUB_MODE_VARS 1
#define SUB_MODE_GRID 2
#define SUB_MODE_FULLGRID 3
#define SUB_MODE_DASH 4

// These functions are for interacting with the teletype hardware, each target
// must provide it's own implementation

// used for TIME and LAST
extern uint32_t tele_get_ticks(void);

// called when M or M.ACT are updated
extern void tele_metro_updated(void);

// called by M.RESET
extern void tele_metro_reset(void);

extern void tele_tr(uint8_t i, int16_t v);
extern void tele_tr_pulse(uint8_t i, int16_t time);
extern void tele_tr_pulse_clear(uint8_t i);
extern void tele_tr_pulse_time(uint8_t i, int16_t time);
extern void tele_cv(uint8_t i, int16_t v, uint8_t s);
extern void tele_cv_slew(uint8_t i, int16_t v);
extern uint16_t tele_get_cv(uint8_t i);
extern void tele_cv_cal(uint8_t n, int32_t b, int32_t m);

extern void tele_update_adc(uint8_t force);

// inform target if there are delays
extern void tele_has_delays(bool has_delays);

// inform target if the stack has entries
extern void tele_has_stack(bool has_stack);

extern void tele_cv_off(uint8_t i, int16_t v);
extern void tele_ii_tx(uint8_t addr, uint8_t* data, uint8_t l);
extern void tele_ii_rx(uint8_t addr, uint8_t* data, uint8_t l);

// send a raw MIDI message (pack[0]=status, pack[1..]=data) to a connected
// USB MIDI device; port selects the USB virtual cable (0=A, 1=B on a
// multi-port interface); len is the message length in bytes (1-3)
extern void tele_midi_out(uint8_t port, uint8_t* pack, uint8_t len);
extern void tele_scene(uint8_t i, uint8_t init_grid, uint8_t init_pattern);

// called when a pattern is updated
extern void tele_pattern_updated(void);

extern void tele_vars_updated(void);

extern void tele_kill(void);
extern void tele_mute(void);
extern bool tele_get_input_state(uint8_t);

void tele_save_calibration(void);

#ifdef TELETYPE_PROFILE
void tele_profile_script(size_t);
void tele_profile_delay(uint8_t);
#endif

// emulate grid key press
extern void grid_key_press(uint8_t x, uint8_t y, uint8_t z);

// manage device config
extern void device_flip(void);

// meadowphysics ops (native engine): channel 0 = all rows, 1-8 = one row
extern void meadowphysics_op_reset(int16_t channel);
extern void meadowphysics_op_stop(int16_t channel);
extern void meadowphysics_op_run(int16_t on);  // 1 = play, 0 = stop

// kria ops (native engine). For get/set pairs, `set` != 0 writes `val`; all
// return the current value. track/param are 0-indexed.
extern void kria_op_run(int16_t on);  // 1 = play, 0 = stop
extern void kria_op_reset(void);
extern int16_t kria_op_pattern(int16_t set, int16_t val);
extern int16_t kria_op_scale(int16_t set, int16_t val);
extern int16_t kria_op_period(int16_t set, int16_t val);
extern int16_t kria_op_mute(int16_t track, int16_t set, int16_t val);
extern void kria_op_tmute(int16_t track);
extern void kria_op_clock(int16_t track);
extern int16_t kria_op_dir(int16_t track, int16_t set, int16_t val);
extern int16_t kria_op_cue(int16_t set, int16_t val);
extern int16_t kria_op_pos(int16_t track, int16_t param, int16_t set,
                           int16_t val);
extern int16_t kria_op_loop_start(int16_t track, int16_t param, int16_t set,
                                  int16_t val);
extern int16_t kria_op_loop_len(int16_t track, int16_t param, int16_t set,
                                int16_t val);
extern int16_t kria_op_cv(int16_t track);
extern int16_t kria_op_dur(int16_t track);
// enable/disable an i2c follower (0-5: JF/TXo/ER301/Disting/WSYN/Crow)
extern int16_t kria_op_ii(int16_t follower, int16_t set, int16_t val);

// earthsea ops (native engine). Semantics mirror Ansible's ii_es handlers;
// see EARTHSEA_PORT_PLAN.md §8. voice is 0-indexed.
extern void es_op_run(int16_t on);     // 1 = play pattern, 0 = stop + silence
extern void es_op_pattern(int16_t p);  // select pattern 0-15
extern void es_op_clock(int16_t d);    // step next chord group (d unused)
extern void es_op_reset(int16_t pos);  // (re)start playback at pos/16 (0-15)
extern void es_op_stop(void);
extern void es_op_trans(int16_t d);      // walk root +/-d, restart playback
extern void es_op_magic(int16_t d);      // 1 half 2 dbl 3/4 linearize 5/6 dir
extern void es_op_mode(int16_t d);       // edge: <0/>15 patt, 0 drone, 1-15 fix
extern int16_t es_op_cv(int16_t voice);  // voice pitch as raw CV (ET)

// live screen / dashboard
extern void set_live_submode(uint8_t submode);
extern void select_dash_screen(uint8_t screen);
extern void print_dashboard_value(uint8_t index, int16_t value);
extern int16_t get_dashboard_value(uint8_t index);

extern void reset_midi_counter(void);

#endif
