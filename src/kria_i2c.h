#ifndef _KRIA_I2C_H_
#define _KRIA_I2C_H_

// i2c follower output for native Kria: routes each track's pitch/gate to
// enabled follower modules (Ansible's i2c-leader feature). Additive to the
// CV/TR jacks. Faithful port of Ansible's src/ansible_ii_leader.c (per-follower
// ops vtable), driving Teletype's i2c bus via tele_ii_tx. See KRIA_I2C_PLAN.md.
//
// Followers (index order = KR_F_*): Just Friends, TELEXo, ER-301, Disting EX,
// W/syn, Crow. track n -> follower voice/output n+1 (per follower).

#include <stdint.h>

#include "kria_engine.h"  // kria_config_t, KRIA_I2C_FOLLOWERS, kria_i2c_fstate_t

typedef struct i2c_follower i2c_follower_t;

typedef struct {
    void (*init)(i2c_follower_t*, uint8_t track, uint8_t state);
    void (*mode)(i2c_follower_t*, uint8_t track, uint8_t mode);
    void (*tr)(i2c_follower_t*, uint8_t track, uint8_t state);
    void (*mute)(i2c_follower_t*, uint8_t track, uint8_t mode);
    void (*cv)(i2c_follower_t*, uint8_t track, uint16_t dac_value);
    void (*octave)(i2c_follower_t*, uint8_t track, int8_t octave);
    void (*slew)(i2c_follower_t*, uint8_t track, uint16_t slew);
    uint8_t mode_ct;
    uint8_t midi;      // MIDI follower (OLED-configured); 0 for CV followers
    uint8_t chan_max;  // MIDI channel count (16 MO / 32 I2M); 0 for non-MIDI
} i2c_ops_t;

// MIDI follower operating modes (active_mode when ops->midi).
#define KR_MIDI_PITCH_SINGLE 0  // all routed tracks -> base chan, pitched
#define KR_MIDI_PITCH_MULTI 1   // track n -> chan+n, pitched
#define KR_MIDI_8T_NOTES 2      // 8 fixed notes[] on base chan (MP 8T)
#define KR_MIDI_8T_CHANS 3      // 1 fixed note (notes[0]) on chans[] (MP 8T)
#define KR_MIDI_MODE_CT 4

struct i2c_follower {
    uint8_t addr;
    uint8_t active;
    uint8_t track_en;  // 8-bit mask of driven tracks/gates
    int8_t oct;
    uint8_t active_mode;
    const i2c_ops_t* ops;
    // MIDI-only state (I2M / MO); ignored by CV followers.
    uint8_t chan;                    // base MIDI channel (0-based)
    uint8_t port;                    // MO USB cable (0=A, 1=B)
    uint8_t notes[KRIA_I2C_TRACKS];  // 8T fixed notes
    uint8_t chans[KRIA_I2C_TRACKS];  // 8T.CHANS per-gate channels
};

// Set the current pitch (semitone index) + aux (duration, for velocity) for a
// track; the follower ops read these when emitting. Call before cv/tr.
void kria_i2c_set_voice(uint8_t track, int16_t semitones, uint16_t aux);

// Fan out a track's CV / gate / slew to every active follower that drives it.
void kria_i2c_cv(uint8_t track, uint16_t dac_value);
void kria_i2c_tr(uint8_t track, uint8_t on);
void kria_i2c_slew(uint8_t track, uint16_t slew);

// Persistence: sync the runtime follower table with the global saved blob.
// load() sanitizes an erased/invalid blob to defaults.
void kria_i2c_load(const kria_i2c_fstate_t* st);
void kria_i2c_save(kria_i2c_fstate_t* st);
void kria_i2c_defaults(kria_i2c_fstate_t* st);

// Config accessors.
i2c_follower_t* kria_i2c_follower(uint8_t index);  // 0..KRIA_I2C_FOLLOWERS-1
void kria_i2c_toggle_active(uint8_t index);
void kria_i2c_set_active(uint8_t index, uint8_t on);  // for KR.II
void kria_i2c_toggle_track(uint8_t index, uint8_t track);
void kria_i2c_set_octave(uint8_t index, int8_t oct);
void kria_i2c_set_mode(uint8_t index, uint8_t mode);

// MIDI follower (I2M/MO) config accessors, used by the OLED editor. Setters
// clamp and mark the bank dirty; index must be a MIDI follower.
void kria_i2c_set_channel(uint8_t index, uint8_t chan);  // base, 0-based
void kria_i2c_set_port(uint8_t index, uint8_t port);     // MO cable 0/1
void kria_i2c_set_note(uint8_t index, uint8_t slot, uint8_t note);  // 8T notes
void kria_i2c_set_chan_slot(uint8_t index, uint8_t slot, uint8_t chan);
uint8_t kria_i2c_is_midi(uint8_t index);   // 1 if follower is I2M/MO
uint8_t kria_i2c_chan_max(uint8_t index);  // MIDI channel count (16/32)

// Shared i2c view (Ansible ii pages), used by both the Kria and MP shells.
// enter() resets the view; render() fills a 16x8 led buffer; key() handles a
// press and returns true if a follower setting changed (caller should mark the
// bank dirty). take_dirty() returns+clears the accumulated edit flag (call at
// view-leave / mode-exit to decide whether to flash).
void kria_i2c_view_enter(void);
void kria_i2c_view_render(uint8_t* led, uint8_t vari);
void kria_i2c_view_key(uint8_t x, uint8_t y, uint8_t z);
uint8_t kria_i2c_take_dirty(void);

// Poll for a pending request to open the OLED MIDI editor (returns the follower
// index, or -1). Set when the user config-taps a MIDI follower on the grid.
int8_t kria_i2c_view_take_oled_req(void);

#endif
