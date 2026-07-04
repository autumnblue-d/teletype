#ifndef _KRIA_I2C_H_
#define _KRIA_I2C_H_

// i2c follower output for native Kria: routes each track's pitch/gate to enabled
// follower modules (Ansible's i2c-leader feature). Additive to the CV/TR jacks.
// Faithful port of Ansible's src/ansible_ii_leader.c (per-follower ops vtable),
// driving Teletype's i2c bus via tele_ii_tx. See KRIA_I2C_PLAN.md.
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
} i2c_ops_t;

struct i2c_follower {
    uint8_t addr;
    uint8_t active;
    uint8_t track_en;    // 4-bit mask of driven tracks
    int8_t oct;
    uint8_t active_mode;
    const i2c_ops_t* ops;
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

// Shared i2c view (Ansible ii pages), used by both the Kria and MP shells.
// enter() resets the view; render() fills a 16x8 led buffer; key() handles a
// press and returns true if a follower setting changed (caller should mark the
// bank dirty). take_dirty() returns+clears the accumulated edit flag (call at
// view-leave / mode-exit to decide whether to flash).
void kria_i2c_view_enter(void);
void kria_i2c_view_render(uint8_t* led, uint8_t vari);
void kria_i2c_view_key(uint8_t x, uint8_t y, uint8_t z);
uint8_t kria_i2c_take_dirty(void);

#endif
