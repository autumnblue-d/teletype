#include <stdint.h>

#include "beta_tests.h"
#include "dejavu_tests.h"
#include "drum_helpers_tests.h"
#include "es_tests.h"
#include "greatest/greatest.h"
#include "grids_helpers_tests.h"
#include "kria_tests.h"
#include "match_token_tests.h"
#include "meadowphysics_tests.h"
#include "midi_out_tests.h"
#include "op_mod_tests.h"
#include "parser_tests.h"
#include "process_tests.h"
#include "serialize_scene_tests.h"
#include "teletype.h"
#include "teletype_io.h"
#include "turtle_tests.h"

uint32_t tele_get_ticks() {
    return 0;
}
void tele_metro_updated() {}
void tele_metro_reset() {}
void tele_tr(uint8_t i, int16_t v) {}
void tele_tr_pulse(uint8_t i, int16_t time) {}
void tele_tr_pulse_clear(uint8_t i) {}
void tele_tr_pulse_time(uint8_t i, int16_t time) {}
void tele_cv(uint8_t i, int16_t v, uint8_t s) {}
void tele_cv_slew(uint8_t i, int16_t v) {}
uint16_t tele_get_cv(uint8_t i) {
    return 0;
}
void tele_update_adc(uint8_t force) {}
void tele_has_delays(bool i) {}
void tele_has_stack(bool i) {}
void tele_cv_off(uint8_t i, int16_t v) {}
void tele_cv_cal(uint8_t i, int32_t b, int32_t m) {}
void tele_ii_tx(uint8_t addr, uint8_t* data, uint8_t l) {}
void tele_ii_rx(uint8_t addr, uint8_t* data, uint8_t l) {}
void tele_scene(uint8_t i, uint8_t init_grid, uint8_t init_pattern) {}
void tele_pattern_updated() {}
void tele_kill() {}
void tele_mute() {}
void tele_vars_updated() {}
void tele_profile_script(size_t s) {}
void tele_profile_delay(uint8_t d) {}
bool tele_get_input_state(uint8_t n) {
    return false;
}
void device_flip() {}
void set_live_submode(uint8_t submode) {}
void select_dash_screen(uint8_t screen) {}
void print_dashboard_value(uint8_t index, int16_t value) {}
int16_t get_dashboard_value(uint8_t index) {
    return 0;
}
void reset_midi_counter() {}
// Capture MIDI-out packets so midi_out_tests can assert on what was sent.
#define TEST_MIDI_OUT_CAP 64
size_t test_midi_out_count = 0;
uint8_t test_midi_out_port[TEST_MIDI_OUT_CAP];
uint8_t test_midi_out_msg[TEST_MIDI_OUT_CAP][3];
void test_midi_out_reset(void) {
    test_midi_out_count = 0;
}
void tele_midi_out(uint8_t port, uint8_t* pack, uint8_t len) {
    if (test_midi_out_count >= TEST_MIDI_OUT_CAP) return;
    test_midi_out_port[test_midi_out_count] = port;
    test_midi_out_msg[test_midi_out_count][0] = pack[0];
    test_midi_out_msg[test_midi_out_count][1] = len > 1 ? pack[1] : 0;
    test_midi_out_msg[test_midi_out_count][2] = len > 2 ? pack[2] : 0;
    test_midi_out_count++;
}
void tele_save_calibration() {}
void grid_key_press(uint8_t x, uint8_t y, uint8_t z) {}
void meadowphysics_op_reset(int16_t channel) {}
void meadowphysics_op_stop(int16_t channel) {}
void meadowphysics_op_run(int16_t on) {}
int16_t meadowphysics_op_sync_get(void) {
    return 0;
}
void meadowphysics_op_sync_set(int16_t src) {}
void meadowphysics_op_clock(void) {}
int16_t meadowphysics_op_voice_get(void) {
    return 0;
}
void meadowphysics_op_voice_set(int16_t mode) {}
int16_t meadowphysics_op_period_get(void) {
    return 0;
}
void meadowphysics_op_period_set(int16_t ms) {}
int16_t meadowphysics_op_scale_get(void) {
    return 0;
}
void meadowphysics_op_scale_set(int16_t slot) {}
int16_t meadowphysics_op_ladder_get(int16_t slot, int16_t degree) {
    return 0;
}
void meadowphysics_op_ladder_set(int16_t slot, int16_t degree, int16_t val) {}
int16_t meadowphysics_op_preset_get(void) {
    return 0;
}
void meadowphysics_op_preset_set(int16_t slot) {}
void kria_op_run(int16_t on) {}
void kria_op_reset(void) {}
int16_t kria_op_pattern(int16_t set, int16_t val) {
    return 0;
}
int16_t kria_op_scale(int16_t set, int16_t val) {
    return 0;
}
int16_t kria_op_period(int16_t set, int16_t val) {
    return 0;
}
// Models per-track mute state so op-level tests can round-trip KR.MUTE and
// catch track/value argument-order regressions (see process_tests.c).
static int16_t test_kria_mute[8];
int16_t kria_op_mute(int16_t track, int16_t set, int16_t val) {
    if (track < 0 || track >= 8) return 0;
    if (set) { test_kria_mute[track] = val; }
    return test_kria_mute[track];
}
void kria_op_tmute(int16_t track) {}
void kria_op_clock(int16_t track) {}
int16_t kria_op_dir(int16_t track, int16_t set, int16_t val) {
    return 0;
}
int16_t kria_op_cue(int16_t set, int16_t val) {
    return 0;
}
int16_t kria_op_pos(int16_t track, int16_t param, int16_t set, int16_t val) {
    return 0;
}
int16_t kria_op_loop_start(int16_t track, int16_t param, int16_t set,
                           int16_t val) {
    return 0;
}
int16_t kria_op_loop_len(int16_t track, int16_t param, int16_t set,
                         int16_t val) {
    return 0;
}
int16_t kria_op_cv(int16_t track) {
    return 0;
}
int16_t kria_op_dur(int16_t track) {
    return 0;
}
int16_t kria_op_ii(int16_t follower, int16_t set, int16_t val) {
    return 0;
}
void es_op_run(int16_t on) {}
void es_op_pattern(int16_t p) {}
void es_op_clock(int16_t d) {}
void es_op_reset(int16_t pos) {}
void es_op_stop(void) {}
void es_op_trans(int16_t d) {}
void es_op_magic(int16_t d) {}
void es_op_mode(int16_t d) {}
int16_t es_op_cv(int16_t voice) {
    return 0;
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();

    RUN_SUITE(match_token_suite);
    RUN_SUITE(op_mod_suite);
    RUN_SUITE(parser_suite);
    RUN_SUITE(process_suite);
    RUN_SUITE(turtle_suite);
    RUN_SUITE(drum_helpers_suite);
    RUN_SUITE(grids_helpers_suite);
    RUN_SUITE(dejavu_suite);
    RUN_SUITE(beta_suite);
    RUN_SUITE(serialize_scene_suite);
    RUN_SUITE(meadowphysics_suite);
    RUN_SUITE(kria_suite);
    RUN_SUITE(es_suite);
    RUN_SUITE(midi_out_suite);

    GREATEST_MAIN_END();
}
