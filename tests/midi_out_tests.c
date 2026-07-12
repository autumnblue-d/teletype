#include "midi_out_tests.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>  // snprintf

#include "greatest/greatest.h"
#include "teletype.h"

// MIDI-out capture, defined in main.c (tele_midi_out stub).
extern size_t test_midi_out_count;
extern uint8_t test_midi_out_port[];
extern uint8_t test_midi_out_msg[][3];
void test_midi_out_reset(void);

// Parse/validate/run one command line against a scene (MO ops return no value).
static TEST mo_cmd(scene_state_t *ss, char *line) {
    exec_state_t es;
    es_init(&es);
    es_push(&es);
    es_variables(&es)->script_number = 0;
    tele_command_t cmd;
    char err[TELE_ERROR_MSG_LENGTH];
    ASSERT_EQ_FMT(E_OK, parse(line, &cmd, err), "%d");
    ASSERT_EQ_FMT(E_OK, validate(&cmd, err), "%d");
    process_command(ss, &es, &cmd);
    PASS();
}

// As mo_cmd, but assert the command returned a value equal to `expected`.
static TEST mo_cmd_val(scene_state_t *ss, char *line, int16_t expected) {
    exec_state_t es;
    es_init(&es);
    es_push(&es);
    es_variables(&es)->script_number = 0;
    tele_command_t cmd;
    char err[TELE_ERROR_MSG_LENGTH];
    ASSERT_EQ_FMT(E_OK, parse(line, &cmd, err), "%d");
    ASSERT_EQ_FMT(E_OK, validate(&cmd, err), "%d");
    process_result_t r = process_command(ss, &es, &cmd);
    ASSERT_EQ(true, r.has_value);
    ASSERT_EQ_FMT(expected, r.value, "%d");
    PASS();
}

// Count captured packets whose status high-nibble matches (e.g. 0x80 off).
static int count_status(uint8_t status_nibble) {
    int n = 0;
    for (size_t i = 0; i < test_midi_out_count; i++)
        if ((test_midi_out_msg[i][0] & 0xF0) == status_nibble) n++;
    return n;
}

TEST test_MO_NG_note_on_then_scheduled_off() {
    scene_state_t ss;
    ss_init(&ss);
    test_midi_out_reset();

    CHECK_CALL(mo_cmd(&ss, "MO.CH 1"));  // pin default channel to 0 (0x90)
    test_midi_out_reset();
    CHECK_CALL(mo_cmd(&ss, "MO.NG 60 100 25"));
    // immediate Note On, channel 0
    ASSERT_EQ_FMT((size_t)1, test_midi_out_count, "%zu");
    ASSERT_EQ_FMT(0x90, test_midi_out_msg[0][0], "%d");
    ASSERT_EQ_FMT(60, test_midi_out_msg[0][1], "%d");
    ASSERT_EQ_FMT(100, test_midi_out_msg[0][2], "%d");

    tele_tick(&ss, 10);  // 15 ms remaining
    ASSERT_EQ_FMT((size_t)1, test_midi_out_count, "%zu");
    tele_tick(&ss, 10);  // 5 ms remaining
    ASSERT_EQ_FMT((size_t)1, test_midi_out_count, "%zu");
    tele_tick(&ss, 10);  // due -> Note Off
    ASSERT_EQ_FMT((size_t)2, test_midi_out_count, "%zu");
    ASSERT_EQ_FMT(0x80, test_midi_out_msg[1][0], "%d");
    ASSERT_EQ_FMT(60, test_midi_out_msg[1][1], "%d");
    ASSERT_EQ_FMT(0, test_midi_out_msg[1][2], "%d");

    PASS();
}

TEST test_MO_TR_uses_20ms_default_gate() {
    scene_state_t ss;
    ss_init(&ss);
    CHECK_CALL(mo_cmd(&ss, "MO.CH 1"));        // pin default channel to 0 (0x90)
    CHECK_CALL(mo_cmd(&ss, "MO.TR.TIME 20"));  // pin gate to the 20 ms default
    test_midi_out_reset();

    CHECK_CALL(mo_cmd(&ss, "MO.TR 40 120"));
    ASSERT_EQ_FMT(0x90, test_midi_out_msg[0][0], "%d");
    ASSERT_EQ_FMT(40, test_midi_out_msg[0][1], "%d");

    tele_tick(&ss, 10);  // 10 ms remaining, not due yet
    ASSERT_EQ_FMT((size_t)1, test_midi_out_count, "%zu");
    tele_tick(&ss, 10);  // 20 ms -> Note Off
    ASSERT_EQ_FMT((size_t)2, test_midi_out_count, "%zu");
    ASSERT_EQ_FMT(0x80, test_midi_out_msg[1][0], "%d");
    ASSERT_EQ_FMT(40, test_midi_out_msg[1][1], "%d");

    PASS();
}

TEST test_MO_TR_TIME_get_set() {
    scene_state_t ss;
    ss_init(&ss);
    CHECK_CALL(mo_cmd(&ss, "MO.CH 1"));

    CHECK_CALL(mo_cmd(&ss, "MO.TR.TIME 40"));   // set
    CHECK_CALL(mo_cmd_val(&ss, "MO.TR.TIME", 40));  // get round-trips

    test_midi_out_reset();
    CHECK_CALL(mo_cmd(&ss, "MO.TR 50 100"));
    tele_tick(&ss, 30);  // 10 ms remaining
    ASSERT_EQ_FMT((size_t)1, test_midi_out_count, "%zu");
    tele_tick(&ss, 10);  // 40 ms -> Note Off
    ASSERT_EQ_FMT((size_t)2, test_midi_out_count, "%zu");
    ASSERT_EQ_FMT(0x80, test_midi_out_msg[1][0], "%d");

    PASS();
}

TEST test_MO_NG_channel_variant() {
    scene_state_t ss;
    ss_init(&ss);
    test_midi_out_reset();

    CHECK_CALL(mo_cmd(&ss, "MO.NG# 3 60 100 10"));  // channel 3 -> 0x92
    ASSERT_EQ_FMT(0x92, test_midi_out_msg[0][0], "%d");
    tele_tick(&ss, 10);
    ASSERT_EQ_FMT(0x82, test_midi_out_msg[1][0], "%d");  // off on channel 3

    PASS();
}

TEST test_MO_NG_retrigger_single_off() {
    scene_state_t ss;
    ss_init(&ss);
    test_midi_out_reset();

    CHECK_CALL(mo_cmd(&ss, "MO.NG 60 100 30"));
    tele_tick(&ss, 10);                             // 20 ms remaining
    CHECK_CALL(mo_cmd(&ss, "MO.NG 60 110 30"));      // retrigger, refresh to 30

    // two Note Ons so far, no Note Off yet
    ASSERT_EQ_FMT(2, count_status(0x90), "%d");
    ASSERT_EQ_FMT(0, count_status(0x80), "%d");

    tele_tick(&ss, 10);
    tele_tick(&ss, 10);
    tele_tick(&ss, 10);  // 30 ms since refresh -> exactly one Note Off
    ASSERT_EQ_FMT(1, count_status(0x80), "%d");

    PASS();
}

TEST test_MO_NALL_flushes_held_notes() {
    scene_state_t ss;
    ss_init(&ss);
    test_midi_out_reset();

    CHECK_CALL(mo_cmd(&ss, "MO.NG 60 100 1000"));
    CHECK_CALL(mo_cmd(&ss, "MO.NG 64 100 1000"));
    ASSERT_EQ_FMT(0, count_status(0x80), "%d");

    CHECK_CALL(mo_cmd(&ss, "MO.NALL"));
    ASSERT_EQ_FMT(2, count_status(0x80), "%d");  // both released

    // nothing left to fire on later ticks
    tele_tick(&ss, 2000);
    ASSERT_EQ_FMT(2, count_status(0x80), "%d");

    PASS();
}

TEST test_MO_overflow_steals_oldest() {
    scene_state_t ss;
    ss_init(&ss);
    test_midi_out_reset();

    // fill all 16 slots with distinct notes, no ticks in between
    char line[24];
    for (int note = 0; note < MIDI_OUT_NOTE_SLOTS; note++) {
        snprintf(line, sizeof(line), "MO.NG %d 100 1000", note);
        CHECK_CALL(mo_cmd(&ss, line));
    }
    ASSERT_EQ_FMT(0, count_status(0x80), "%d");  // none released yet

    // 17th note: pool full -> steal the slot nearest firing (note 0), which
    // means an early Note Off for note 0 right now.
    CHECK_CALL(mo_cmd(&ss, "MO.NG 100 100 1000"));
    ASSERT_EQ_FMT(1, count_status(0x80), "%d");
    // the released note must be note 0
    int off_note = -1;
    for (size_t i = 0; i < test_midi_out_count; i++)
        if ((test_midi_out_msg[i][0] & 0xF0) == 0x80)
            off_note = test_midi_out_msg[i][1];
    ASSERT_EQ_FMT(0, off_note, "%d");

    PASS();
}

TEST test_MO_PB_packs_two_7bit_bytes() {
    scene_state_t ss;
    ss_init(&ss);
    CHECK_CALL(mo_cmd(&ss, "MO.CH 1"));  // pin default channel to 0 (0xE0)

    // centre (8192): LSB 0, MSB 64
    test_midi_out_reset();
    CHECK_CALL(mo_cmd(&ss, "MO.PB 8192"));
    ASSERT_EQ_FMT(0xE0, test_midi_out_msg[0][0], "%d");
    ASSERT_EQ_FMT(0, test_midi_out_msg[0][1], "%d");
    ASSERT_EQ_FMT(64, test_midi_out_msg[0][2], "%d");

    // max (16383): both bytes 0x7F
    test_midi_out_reset();
    CHECK_CALL(mo_cmd(&ss, "MO.PB 16383"));
    ASSERT_EQ_FMT(0x7F, test_midi_out_msg[0][1], "%d");
    ASSERT_EQ_FMT(0x7F, test_midi_out_msg[0][2], "%d");

    // 200: both data bytes must stay in valid 0..127 range (regression:
    // the old code emitted 200 as a data byte, i.e. a stray status byte)
    test_midi_out_reset();
    CHECK_CALL(mo_cmd(&ss, "MO.PB 200"));
    ASSERT(test_midi_out_msg[0][1] < 0x80);
    ASSERT(test_midi_out_msg[0][2] < 0x80);
    ASSERT_EQ_FMT(200 & 0x7F, test_midi_out_msg[0][1], "%d");
    ASSERT_EQ_FMT(200 >> 7, test_midi_out_msg[0][2], "%d");

    PASS();
}

SUITE(midi_out_suite) {
    RUN_TEST(test_MO_NG_note_on_then_scheduled_off);
    RUN_TEST(test_MO_TR_uses_20ms_default_gate);
    RUN_TEST(test_MO_TR_TIME_get_set);
    RUN_TEST(test_MO_NG_channel_variant);
    RUN_TEST(test_MO_NG_retrigger_single_off);
    RUN_TEST(test_MO_NALL_flushes_held_notes);
    RUN_TEST(test_MO_overflow_steals_oldest);
    RUN_TEST(test_MO_PB_packs_two_7bit_bytes);
}
