// Kria i2c follower output -- faithful port of Ansible's ansible_ii_leader.c.
// See kria_i2c.h. i2c_leader_tx -> tele_ii_tx; outputs[t].semitones ->
// kri2c_sem[t]; aux_param[0][t] -> kri2c_aux[t]; ET[] via music.h (clamped).

#include "kria_i2c.h"

#include <string.h>

#include "ii.h"
#include "music.h"        // ET
#include "teletype_io.h"  // tele_ii_tx

// current pitch (semitone index) + aux (duration) per track, set by the shell
static int16_t kri2c_sem[KRIA_NUM_TRACKS];
static uint16_t kri2c_aux[KRIA_NUM_TRACKS];
static uint8_t view_dirty = 0;  // a follower setting changed since last flush

static uint16_t et(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    return ET[idx];
}

static uint16_t aux_to_vel(uint16_t aux) {
    // map the duration param to ~V2-V5 for velocity (Ansible aux_to_vel)
    return aux * 41 + 3264;
}

// ---- Just Friends ----

static void ii_init_jf(i2c_follower_t* f, uint8_t track, uint8_t state) {
    (void)track;
    uint8_t d[4] = { 0 };
    if (!state) {
        d[0] = JF_VTR;
        d[1] = 0;
        d[2] = 16384 >> 8;
        d[3] = 16384 & 0xff;
        tele_ii_tx(f->addr, d, 3);
        d[0] = JF_TR;
        d[1] = 0;
        d[2] = 0;
        tele_ii_tx(f->addr, d, 3);
    }
    d[0] = JF_MODE;
    d[1] = state;
    tele_ii_tx(f->addr, d, 2);
}

static void ii_tr_jf(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[6] = { 0 };
    uint8_t l = 0;
    uint16_t pitch = et(kri2c_sem[track]);
    if (state) {
        uint16_t vel = aux_to_vel(kri2c_aux[track]);
        switch (f->active_mode) {
            case 0:  // polyphonically allocated
                d[0] = JF_NOTE;
                d[1] = pitch >> 8;
                d[2] = pitch & 0xff;
                d[3] = vel >> 8;
                d[4] = vel & 0xff;
                l = 5;
                break;
            case 1:  // tracks to first 4 voices
                d[0] = JF_VOX;
                d[1] = track + 1;
                d[2] = pitch >> 8;
                d[3] = pitch & 0xff;
                d[4] = vel >> 8;
                d[5] = vel & 0xff;
                l = 6;
                break;
            case 2:  // envelopes
                d[0] = JF_VTR;
                d[1] = track + 1;
                d[2] = vel >> 8;
                d[3] = vel & 0xff;
                l = 4;
                break;
            default: return;
        }
    }
    else {
        if (f->active_mode == 0) {
            d[0] = JF_NOTE;
            d[1] = pitch >> 8;
            d[2] = pitch & 0xff;
            d[3] = 0;
            d[4] = 0;
            l = 5;
        }
        else {
            d[0] = JF_TR;
            d[1] = track + 1;
            d[2] = 0;
            l = 3;
        }
    }
    if (l) tele_ii_tx(f->addr, d, l);
}

static void ii_mute_jf(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    uint8_t d[3] = { JF_TR, 0, 0 };
    tele_ii_tx(f->addr, d, 3);
}

static void ii_mode_jf(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    if (mode > f->ops->mode_ct) return;
    f->active_mode = mode;
    uint8_t d[4] = { 0 };
    if (mode == 2) {
        d[0] = JF_MODE;
        d[1] = 0;
        tele_ii_tx(f->addr, d, 2);
        d[0] = JF_TR;
        d[1] = 0;
        d[2] = 0;
        d[3] = 0;
        tele_ii_tx(f->addr, d, 3);
    }
    else {
        d[0] = JF_MODE;
        d[1] = 1;
        tele_ii_tx(f->addr, d, 2);
    }
}

static void ii_octave_jf(i2c_follower_t* f, uint8_t track, int8_t octave) {
    (void)track;
    int16_t shift;
    if (octave > 0)
        shift = et(12 * octave);
    else if (octave < 0)
        shift = -(int16_t)et(12 * (-octave));
    else
        shift = 0;
    uint8_t d[] = { JF_SHIFT, shift >> 8, shift & 0xff };
    tele_ii_tx(f->addr, d, 3);
}

// ---- TELEXo (also reused by ER-301: same TO_TR/TO_CV command bytes) ----

static void ii_init_txo(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[4] = { 0 };
    if (state == 0) {
        d[0] = 0x60;  // TO_ENV_ACT
        d[1] = track;
        tele_ii_tx(f->addr, d, 4);
        d[0] = 0x40;  // TO_OSC
        d[1] = track;
        tele_ii_tx(f->addr, d, 4);
        d[0] = 0x10;  // TO_CV
        d[1] = track;
        tele_ii_tx(f->addr, d, 4);
    }
}

static void ii_mode_txo(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    if (mode > f->ops->mode_ct) return;
    f->active_mode = mode;
    uint8_t d[4] = { 0 };
    switch (mode) {
        case 0:  // enveloped oscillators
            d[0] = 0x60;
            d[1] = track;
            d[2] = 0;
            d[3] = 1;
            tele_ii_tx(f->addr, d, 4);
            d[0] = 0x15;  // TO_CV_OFF
            d[1] = track;
            d[2] = 0;
            d[3] = 0;
            tele_ii_tx(f->addr, d, 4);
            d[0] = 0x10;  // TO_CV
            d[1] = track;
            d[2] = 8192 >> 8;
            d[3] = 8192 & 0xff;
            tele_ii_tx(f->addr, d, 4);
            break;
        case 1:  // gate/cv
            d[0] = 0x60;
            d[1] = track;
            tele_ii_tx(f->addr, d, 4);
            d[0] = 0x40;
            d[1] = track;
            tele_ii_tx(f->addr, d, 4);
            d[0] = 0x10;
            d[1] = track;
            tele_ii_tx(f->addr, d, 4);
            break;
        default: return;
    }
}

static void ii_octave_txo(i2c_follower_t* f, uint8_t track, int8_t octave) {
    (void)track;
    int16_t shift;
    switch (f->active_mode) {
        case 0: break;  // osc: pitch computed from oct in cv
        case 1: {
            if (octave > 0)
                shift = et(12 * octave);
            else if (octave < 0)
                shift = -(int16_t)et(12 * (-octave));
            else
                shift = 0;
            uint8_t d[] = { 0x15, 0, shift >> 8, shift & 0xff };  // TO_CV_OFF
            for (uint8_t i = 0; i < 4; i++) {
                d[1] = i;
                tele_ii_tx(f->addr, d, 4);
            }
            break;
        }
        default: return;
    }
}

static void ii_tr_txo(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[4] = { 0 };
    switch (f->active_mode) {
        case 0:           // enveloped oscillator
            d[0] = 0x6D;  // TO_ENV
            d[1] = track;
            d[2] = 0;
            d[3] = state;
            tele_ii_tx(f->addr, d, 4);
            break;
        case 1:           // gate/cv
            d[0] = 0x00;  // TO_TR
            d[1] = track;
            d[2] = 0;
            d[3] = state;
            tele_ii_tx(f->addr, d, 4);
            break;
        default: return;
    }
}

static void ii_mute_txo(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    for (uint8_t i = 0; i < 4; i++) ii_tr_txo(f, i, 0);
}

static void ii_cv_txo(i2c_follower_t* f, uint8_t track, uint16_t dac_value) {
    uint8_t d[4] = { 0 };
    switch (f->active_mode) {
        case 0:  // enveloped oscillator
            dac_value = (uint16_t)((int)dac_value + (int)et(12 * (4 + f->oct)));
            d[0] = 0x40;  // TO_OSC
            d[1] = track;
            d[2] = dac_value >> 8;
            d[3] = dac_value & 0xff;
            tele_ii_tx(f->addr, d, 4);
            break;
        case 1:           // gate/cv
            d[0] = 0x10;  // TO_CV
            d[1] = track;
            d[2] = dac_value >> 8;
            d[3] = dac_value & 0xff;
            tele_ii_tx(f->addr, d, 4);
            break;
        default: return;
    }
}

static void ii_slew_txo(i2c_follower_t* f, uint8_t track, uint16_t slew) {
    uint8_t d[4] = { 0 };
    switch (f->active_mode) {
        case 0:
            d[0] = 0x4F;  // TO_OSC_SLEW
            d[1] = track;
            d[2] = slew >> 8;
            d[3] = slew & 0xff;
            tele_ii_tx(f->addr, d, 4);
            break;
        case 1:
            d[0] = 0x12;  // TO_CV_SLEW
            d[1] = track;
            d[2] = slew >> 8;
            d[3] = slew & 0xff;
            tele_ii_tx(f->addr, d, 4);
            break;
        default: return;
    }
}

// ---- Disting EX ----

static void ii_mode_disting_ex(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    if (mode > f->ops->mode_ct) return;
    f->active_mode = mode;
}

static void ii_tr_disting_ex(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[4] = { 0 };
    int note = kri2c_sem[track] + 12 * (4 + f->oct);
    switch (f->active_mode) {
        case 0:  // SD Multisample / triggers, allocated voices
            if (note < 0)
                note = 0;
            else if (note > 127)
                note = 127;
            if (state) {
                d[0] = 0x56;
                d[1] = note;
                tele_ii_tx(f->addr, d, 2);
                d[0] = 0x55;
                d[2] = 0x40;
                d[3] = 0;
                tele_ii_tx(f->addr, d, 4);
            }
            else {
                d[0] = 0x56;
                d[1] = note;
                tele_ii_tx(f->addr, d, 2);
            }
            break;
        case 1:  // fixed voices
            if (state) {
                d[0] = 0x52;
                d[1] = track;
                d[2] = 0x40;
                d[3] = 0;
                tele_ii_tx(f->addr, d, 4);
            }
            else {
                d[0] = 0x53;
                d[1] = track;
                tele_ii_tx(f->addr, d, 2);
            }
            break;
        case 2:  // MIDI channel 1
            if (note < 0 || note > 127) return;
            d[0] = 0x4F;
            d[1] = state ? 0x90 : 0x80;
            d[2] = note;
            d[3] = state ? 80 : 0;
            tele_ii_tx(f->addr, d, 4);
            break;
        case 3:  // MIDI channels 1-4
            if (note < 0 || note > 127) return;
            d[0] = 0x4F;
            d[1] = (state ? 0x90 : 0x80) + track;
            d[2] = note;
            d[3] = state ? 80 : 0;
            tele_ii_tx(f->addr, d, 4);
            break;
        default: return;
    }
}

static void ii_mute_disting_ex(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    for (uint8_t i = 0; i < 4; i++) ii_tr_disting_ex(f, i, 0);
    uint8_t d[1] = { 0x57 };
    tele_ii_tx(f->addr, d, 1);
}

static void ii_cv_disting_ex(i2c_follower_t* f, uint8_t track,
                             uint16_t dac_value) {
    (void)dac_value;
    uint8_t d[4] = { 0 };
    int note = kri2c_sem[track] + 12 * (4 + f->oct);
    uint16_t pitch = et(note) - et(36);
    d[2] = pitch >> 8;
    d[3] = pitch;
    if (f->active_mode == 0) {
        if (note < 0)
            note = 0;
        else if (note > 127)
            note = 127;
        d[0] = 0x54;
        d[1] = note;
        tele_ii_tx(f->addr, d, 4);
    }
    else if (f->active_mode == 1) {
        d[0] = 0x51;
        d[1] = track;
        tele_ii_tx(f->addr, d, 4);
    }
}

// ---- W/syn ----

static void ii_init_wsyn(i2c_follower_t* f, uint8_t track, uint8_t state) {
    (void)track;
    uint8_t d[4] = { 0 };
    if (!state) {
        d[0] = WS_S_VEL;
        tele_ii_tx(f->addr, d, 4);
    }
    else {
        d[0] = WS_S_AR_MODE;
        d[1] = 0;
        tele_ii_tx(f->addr, d, 2);
    }
}

static void ii_tr_wsyn(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[6] = { 0 };
    uint8_t l = 0;
    uint16_t pitch = et(kri2c_sem[track] + (3 + f->oct) * 12);
    uint16_t vel = state ? aux_to_vel(kri2c_aux[track]) : 0;
    switch (f->active_mode) {
        case 0:  // polyphonically allocated
            d[0] = WS_S_NOTE;
            d[1] = pitch >> 8;
            d[2] = pitch & 0xff;
            d[3] = vel >> 8;
            d[4] = vel & 0xff;
            l = 5;
            break;
        case 1:  // tracks to first 4 voices
            d[0] = WS_S_VOX;
            d[1] = track + 1;
            d[2] = pitch >> 8;
            d[3] = pitch & 0xff;
            d[4] = vel >> 8;
            d[5] = vel & 0xff;
            l = 6;
            break;
        default: return;
    }
    if (l) tele_ii_tx(f->addr, d, l);
}

static void ii_mode_wsyn(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    if (mode > f->ops->mode_ct) return;
    f->active_mode = mode;
}

static void ii_mute_wsyn(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    uint8_t d[4] = { WS_S_VEL, 0, 0, 0 };
    tele_ii_tx(f->addr, d, 4);
}

static void ii_cv_wsyn(i2c_follower_t* f, uint8_t track, uint16_t dac_value) {
    uint8_t d[4] = { WS_S_PITCH, track, dac_value >> 8, dac_value & 0xff };
    tele_ii_tx(f->addr, d, 4);
}

// ---- Crow ----

static void ii_mode_crow(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    if (mode > f->ops->mode_ct) return;
    f->active_mode = mode;
}

static void ii_tr_crow(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t d[7];
    uint16_t pitch = et(kri2c_sem[track]);
    uint16_t vel = state ? aux_to_vel(kri2c_aux[track]) : 0;
    d[0] = 6;  // CROW call3
    d[1] = 0;
    d[2] = track + 1;
    d[3] = pitch >> 8;
    d[4] = pitch & 0xff;
    d[5] = vel >> 8;
    d[6] = vel & 0xff;
    tele_ii_tx(f->addr, d, 7);
}

// ---- no-op ops ----

static void ii_u8_nop(i2c_follower_t* f, uint8_t track, uint8_t state) {
    (void)f;
    (void)track;
    (void)state;
}
static void ii_s8_nop(i2c_follower_t* f, uint8_t track, int8_t state) {
    (void)f;
    (void)track;
    (void)state;
}
static void ii_u16_nop(i2c_follower_t* f, uint8_t track, uint16_t v) {
    (void)f;
    (void)track;
    (void)v;
}

// ---- MIDI followers: I2M (i2c2midi over i2c) + MO (native USB MIDI) ----

#define I2C2MIDI 0x3F    // i2c2midi module address (see src/ops/i2c2midi.c)
#define KR_MIDI_VEL 100  // fixed note velocity (velocity-from-duration = TODO)

// General MIDI drum map for the 8T fixed-note defaults (kick/snare/hats/...).
static const uint8_t GM_DRUM[KRIA_I2C_TRACKS] = {
    36, 38, 42, 46, 39, 45, 49, 51
};

// pitched note for a track (modes 0/1): Kria semitone + base 4 octaves +
// offset, matching the Disting-EX MIDI modes.
static int midi_pitched_note(i2c_follower_t* f, uint8_t track) {
    uint8_t t = track < KRIA_NUM_TRACKS ? track : KRIA_NUM_TRACKS - 1;
    return kri2c_sem[t] + 12 * (4 + f->oct);
}

// Resolve (channel, note) for a track given the follower's MIDI mode. Returns 0
// if this track/mode produces no note (out-of-range pitch).
static uint8_t midi_resolve(i2c_follower_t* f, uint8_t track, uint8_t* ch_out,
                            uint8_t* note_out) {
    uint8_t chmax = f->ops->chan_max ? f->ops->chan_max : 16;
    uint8_t g = track & (KRIA_I2C_TRACKS - 1);
    int note, ch;
    switch (f->active_mode) {
        case KR_MIDI_PITCH_SINGLE:
            note = midi_pitched_note(f, track);
            if (note < 0 || note > 127) return 0;
            ch = f->chan;
            break;
        case KR_MIDI_PITCH_MULTI:
            note = midi_pitched_note(f, track);
            if (note < 0 || note > 127) return 0;
            ch = f->chan + track;
            break;
        case KR_MIDI_8T_NOTES:
            note = f->notes[g];
            ch = f->chan;
            break;
        case KR_MIDI_8T_CHANS:
            note = f->notes[0];
            ch = f->chans[g];
            break;
        default: return 0;
    }
    if (ch < 0) ch = 0;
    if (ch >= chmax) ch = chmax - 1;
    *ch_out = (uint8_t)ch;
    *note_out = (uint8_t)note;
    return 1;
}

static void ii_tr_i2m(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t ch, note;
    if (!midi_resolve(f, track, &ch, &note)) return;
    uint8_t d[4];
    if (state) {  // i2c2midi note-on = cmd 20 (ch, note, vel)
        d[0] = 20;
        d[1] = ch;
        d[2] = note;
        d[3] = KR_MIDI_VEL;
        tele_ii_tx(f->addr, d, 4);
    }
    else {  // note-off = cmd 21 (ch, note)
        d[0] = 21;
        d[1] = ch;
        d[2] = note;
        tele_ii_tx(f->addr, d, 3);
    }
}

static void ii_tr_mo(i2c_follower_t* f, uint8_t track, uint8_t state) {
    uint8_t ch, note;
    if (!midi_resolve(f, track, &ch, &note)) return;
    uint8_t pack[3];
    pack[0] = (state ? 0x90 : 0x80) | (ch & 0x0f);  // note-on/off + channel
    pack[1] = note;
    pack[2] = state ? KR_MIDI_VEL : 0;
    tele_midi_out(f->port, pack, 3);
}

// Mute: note-off every gate (harmless for unrouted ones); notes are stable in
// all supported modes, so recomputing here matches what was sent.
static void ii_mute_i2m(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    for (uint8_t t = 0; t < KRIA_I2C_TRACKS; t++) ii_tr_i2m(f, t, 0);
}
static void ii_mute_mo(i2c_follower_t* f, uint8_t track, uint8_t mode) {
    (void)track;
    (void)mode;
    for (uint8_t t = 0; t < KRIA_I2C_TRACKS; t++) ii_tr_mo(f, t, 0);
}

static void ii_init_midi(i2c_follower_t* f, uint8_t track, uint8_t state) {
    (void)track;
    if (!state) f->ops->mute(f, 0, 0);  // all-notes-off on disable
}

// ---- follower table (order = KR_F_*) ----

static const i2c_ops_t jf_ops = { ii_init_jf, ii_mode_jf, ii_tr_jf,
                                  ii_mute_jf, ii_u16_nop, ii_octave_jf,
                                  ii_u16_nop, 3 };
static const i2c_ops_t txo_ops = { ii_init_txo, ii_mode_txo,
                                   ii_tr_txo,   ii_mute_txo,
                                   ii_cv_txo,   ii_octave_txo,
                                   ii_slew_txo, 2 };
static const i2c_ops_t er301_ops = { ii_u8_nop,   ii_u8_nop, ii_tr_txo,
                                     ii_mute_txo, ii_cv_txo, ii_octave_txo,
                                     ii_slew_txo, 1 };
static const i2c_ops_t disting_ops = { ii_u8_nop,        ii_mode_disting_ex,
                                       ii_tr_disting_ex, ii_mute_disting_ex,
                                       ii_cv_disting_ex, ii_s8_nop,
                                       ii_u16_nop,       4 };
static const i2c_ops_t wsyn_ops = { ii_init_wsyn, ii_mode_wsyn,
                                    ii_tr_wsyn,   ii_mute_wsyn,
                                    ii_cv_wsyn,   ii_s8_nop,
                                    ii_u16_nop,   2 };
static const i2c_ops_t crow_ops = { ii_u8_nop,  ii_mode_crow,
                                    ii_tr_crow, ii_u8_nop,
                                    ii_u16_nop, ii_s8_nop,
                                    ii_u16_nop, 1 };
// MIDI followers: mode handled specially in kria_i2c_set_mode; cv/slew/octave
// are no-ops (pitch rides in the note-on, octave read live in midi_resolve).
static const i2c_ops_t i2m_ops = {
    ii_init_midi, ii_u8_nop,  ii_tr_i2m,       ii_mute_i2m, ii_u16_nop,
    ii_s8_nop,    ii_u16_nop, KR_MIDI_MODE_CT, 1,           32
};
static const i2c_ops_t mo_ops = {
    ii_init_midi, ii_u8_nop,  ii_tr_mo,        ii_mute_mo, ii_u16_nop,
    ii_s8_nop,    ii_u16_nop, KR_MIDI_MODE_CT, 1,          16
};

static i2c_follower_t followers[KRIA_I2C_FOLLOWERS] = {
    { JF_ADDR, 0, 0x0f, 0, 0, &jf_ops },
    { TELEXO_0, 0, 0x0f, 0, 0, &txo_ops },
    { ER301_1, 0, 0x0f, 0, 1, &er301_ops },
    { DISTING_EX_1, 0, 0x0f, 0, 0, &disting_ops },
    { WS_S_ADDR, 0, 0x0f, -2, 0, &wsyn_ops },
    { CROW, 0, 0x0f, 0, 0, &crow_ops },
    { I2C2MIDI, 0, 0x0f, 0, 0, &i2m_ops },
    { 0, 0, 0x0f, 0, 0, &mo_ops },  // MO: native USB MIDI, no i2c address
};

// ---- driving the followers ----

void kria_i2c_set_voice(uint8_t track, int16_t semitones, uint16_t aux) {
    if (track >= KRIA_NUM_TRACKS) return;
    kri2c_sem[track] = semitones;
    kri2c_aux[track] = aux;
}

void kria_i2c_cv(uint8_t track, uint16_t dac_value) {
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++)
        if (followers[i].active && (followers[i].track_en & (1 << track)))
            followers[i].ops->cv(&followers[i], track, dac_value);
}

void kria_i2c_tr(uint8_t track, uint8_t on) {
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++)
        if (followers[i].active && (followers[i].track_en & (1 << track)))
            followers[i].ops->tr(&followers[i], track, on);
}

void kria_i2c_slew(uint8_t track, uint16_t slew) {
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++)
        if (followers[i].active && (followers[i].track_en & (1 << track)))
            followers[i].ops->slew(&followers[i], track, slew);
}

// ---- config edits ----

static void follower_change_mode(i2c_follower_t* f, uint8_t param) {
    for (uint8_t i = 0; i < 4; i++)
        if (f->track_en & (1 << i)) f->ops->mode(f, i, param);
}
static void follower_change_octave(i2c_follower_t* f, int8_t param) {
    for (uint8_t i = 0; i < 4; i++)
        if (f->track_en & (1 << i)) f->ops->octave(f, i, param);
}

i2c_follower_t* kria_i2c_follower(uint8_t index) {
    return index < KRIA_I2C_FOLLOWERS ? &followers[index] : &followers[0];
}

void kria_i2c_toggle_active(uint8_t index) {
    if (index >= KRIA_I2C_FOLLOWERS) return;
    i2c_follower_t* f = &followers[index];
    f->active = !f->active;
    f->ops->init(f, 0, f->active);
    if (f->active) {
        follower_change_mode(f, f->active_mode);
        follower_change_octave(f, f->oct);
    }
    view_dirty = 1;
}

void kria_i2c_toggle_track(uint8_t index, uint8_t track) {
    if (index >= KRIA_I2C_FOLLOWERS || track >= KRIA_I2C_TRACKS) return;
    followers[index].track_en ^= (1 << track);
    view_dirty = 1;
}

void kria_i2c_set_octave(uint8_t index, int8_t oct) {
    if (index >= KRIA_I2C_FOLLOWERS) return;
    followers[index].oct = oct;
    follower_change_octave(&followers[index], oct);
    view_dirty = 1;
}

void kria_i2c_set_mode(uint8_t index, uint8_t mode) {
    if (index >= KRIA_I2C_FOLLOWERS) return;
    if (followers[index].ops->midi) {  // no per-track i2c setup; just latch it
        if (mode < KR_MIDI_MODE_CT) followers[index].active_mode = mode;
        view_dirty = 1;
        return;
    }
    follower_change_mode(&followers[index], mode);
}

// ---- MIDI follower (I2M/MO) config accessors ----

uint8_t kria_i2c_is_midi(uint8_t index) {
    return index < KRIA_I2C_FOLLOWERS && followers[index].ops->midi;
}

uint8_t kria_i2c_chan_max(uint8_t index) {
    if (index >= KRIA_I2C_FOLLOWERS) return 16;
    uint8_t m = followers[index].ops->chan_max;
    return m ? m : 16;
}

void kria_i2c_set_channel(uint8_t index, uint8_t chan) {
    if (!kria_i2c_is_midi(index)) return;
    uint8_t m = kria_i2c_chan_max(index);
    followers[index].chan = chan >= m ? m - 1 : chan;
    view_dirty = 1;
}

void kria_i2c_set_port(uint8_t index, uint8_t port) {
    if (!kria_i2c_is_midi(index)) return;
    followers[index].port = port ? 1 : 0;
    view_dirty = 1;
}

void kria_i2c_set_note(uint8_t index, uint8_t slot, uint8_t note) {
    if (!kria_i2c_is_midi(index) || slot >= KRIA_I2C_TRACKS) return;
    followers[index].notes[slot] = note > 127 ? 127 : note;
    view_dirty = 1;
}

void kria_i2c_set_chan_slot(uint8_t index, uint8_t slot, uint8_t chan) {
    if (!kria_i2c_is_midi(index) || slot >= KRIA_I2C_TRACKS) return;
    uint8_t m = kria_i2c_chan_max(index);
    followers[index].chans[slot] = chan >= m ? m - 1 : chan;
    view_dirty = 1;
}

void kria_i2c_set_active(uint8_t index, uint8_t on) {
    if (index >= KRIA_I2C_FOLLOWERS) return;
    if ((followers[index].active != 0) != (on != 0))
        kria_i2c_toggle_active(index);
}

// ---- persistence (global blob) ----

void kria_i2c_defaults(kria_i2c_fstate_t* st) {
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++) {
        st[i].active = 0;
        st[i].track_en = 0x0f;
        st[i].oct = 0;
        st[i].mode = 0;
        st[i].chan = 0;
        st[i].port = 0;
        for (uint8_t j = 0; j < KRIA_I2C_TRACKS; j++) {
            st[i].notes[j] = GM_DRUM[j];  // 8T fixed-note defaults
            st[i].chans[j] = j;           // 8T.CHANS: gate n -> channel n
        }
    }
    st[KR_F_ER301].mode = 1;  // ER-301 always gate/cv
    st[KR_F_WSYN].oct = -2;   // W/syn
}

void kria_i2c_load(const kria_i2c_fstate_t* st) {
    kria_i2c_fstate_t def[KRIA_I2C_FOLLOWERS];
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++) {
        // sanitize an erased blob (0xFF after chip erase) -> defaults. track_en
        // is now 8-bit (can be 0xff), so the erased sentinel is active > 1.
        if (st[i].active > 1) {
            kria_i2c_defaults(def);
            st = def;
            break;
        }
    }
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++) {
        followers[i].active = st[i].active;
        followers[i].track_en = st[i].track_en;
        followers[i].oct = st[i].oct;
        followers[i].active_mode = st[i].mode;
        followers[i].chan = st[i].chan;
        followers[i].port = st[i].port;
        memcpy(followers[i].notes, st[i].notes, KRIA_I2C_TRACKS);
        memcpy(followers[i].chans, st[i].chans, KRIA_I2C_TRACKS);
    }
}

void kria_i2c_save(kria_i2c_fstate_t* st) {
    for (uint8_t i = 0; i < KRIA_I2C_FOLLOWERS; i++) {
        st[i].active = followers[i].active;
        st[i].track_en = followers[i].track_en;
        st[i].oct = followers[i].oct;
        st[i].mode = followers[i].active_mode;
        st[i].chan = followers[i].chan;
        st[i].port = followers[i].port;
        memcpy(st[i].notes, followers[i].notes, KRIA_I2C_TRACKS);
        memcpy(st[i].chans, followers[i].chans, KRIA_I2C_TRACKS);
    }
}

// ---- shared i2c view (Ansible ii pages: toggle + per-follower config) ----

#define KM_LB 12  // bright / on
#define KM_LD 4   // dim / off

static int8_t view_sel = -1;  // follower being configured (-1 = toggle page)
static uint8_t view_mod = 0;  // (5,7) modifier held (enter config on tap)
static int8_t oled_req =
    -1;  // MIDI follower to open in the OLED editor (-1 none)

void kria_i2c_view_enter(void) {
    view_sel = -1;
    view_mod = 0;
    oled_req = -1;
}

// Consume a pending "open the OLED editor for this MIDI follower" request set
// when the user config-taps I2M/MO on the grid. Polled by the mode shell.
int8_t kria_i2c_view_take_oled_req(void) {
    int8_t r = oled_req;
    oled_req = -1;
    return r;
}

uint8_t kria_i2c_take_dirty(void) {
    uint8_t d = view_dirty;
    view_dirty = 0;
    return d;
}

// follower index at cell (x,y), or -1
static int8_t view_at(uint8_t x, uint8_t y) {
    if (y < 2 || y > 5) return -1;
    int8_t f = (x == 5) ? (y - 2) : (x == 6) ? (y - 2 + 4) : -1;
    return (f >= 0 && f < KRIA_I2C_FOLLOWERS) ? f : -1;
}

void kria_i2c_view_render(uint8_t* led, uint8_t vari) {
    uint8_t i;
    memset(led, 0, 128);

    for (i = 0; i < KRIA_I2C_FOLLOWERS; i++) {
        uint8_t cell = 5 + (i / 4) + (2 + i % 4) * 16;
        if (view_sel >= 0)
            led[cell] = (i == view_sel) ? KM_LB : KM_LD;
        else
            led[cell] = followers[i].active ? KM_LB : KM_LD;
    }
    led[112 + 5] = view_mod ? KM_LB : KM_LD;  // (5,7) config modifier

    if (view_sel >= 0) {
        i2c_follower_t* f = &followers[view_sel];
        for (i = 0; i < KRIA_NUM_TRACKS; i++)  // per-track routing (row 7)
            led[112 + i] = (f->track_en & (1 << i)) ? KM_LB : KM_LD;
        memset(led, KM_LD, 7);  // octave selector (row 0, cols 0-6)
        led[f->oct + 3] = KM_LB;
        if (f->ops->mode_ct > 1) {  // operating mode (row 0, cols 12+)
            memset(led + 12, KM_LD, f->ops->mode_ct);
            led[12 + f->active_mode] = KM_LB;
        }
    }

    for (i = 0; i < 128; i++) {
        if (led[i] > 15) led[i] = 15;
        if (!vari && led[i]) led[i] = 15;
    }
}

void kria_i2c_view_key(uint8_t x, uint8_t y, uint8_t z) {
    if (!z) {
        if (x == 5 && y == 7) view_mod = 0;
        return;
    }
    if (x == 5 && y == 7) {
        if (view_sel >= 0)
            view_sel = -1;  // exit config
        else
            view_mod = 1;  // arm config modifier
        return;
    }
    if (view_sel >= 0) {  // config page
        i2c_follower_t* f = &followers[view_sel];
        int8_t sw = view_at(x, y);
        if (sw >= 0) {
            if (kria_i2c_is_midi(sw)) {  // MIDI -> hand off to the OLED editor
                oled_req = sw;
                view_sel = -1;
            }
            else
                view_sel = sw;  // switch configured follower
        }
        else if (y == 0 && x <= 6) {
            kria_i2c_set_octave(view_sel, (int8_t)(x - 3));
            view_dirty = 1;
        }
        else if (y == 0 && f->ops->mode_ct > 1 && x >= 12 &&
                 x < 12 + f->ops->mode_ct) {
            kria_i2c_set_mode(view_sel, x - 12);
            view_dirty = 1;
        }
        else if (y == 7 && x < KRIA_NUM_TRACKS) {
            kria_i2c_toggle_track(view_sel, x);
            view_dirty = 1;
        }
    }
    else {  // toggle page
        int8_t f = view_at(x, y);
        if (f >= 0) {
            if (view_mod) {
                if (kria_i2c_is_midi(f))
                    oled_req = f;  // MIDI -> OLED editor
                else
                    view_sel = f;  // CV -> grid config page
            }
            else {
                kria_i2c_toggle_active(f);
                view_dirty = 1;
            }
        }
    }
}
