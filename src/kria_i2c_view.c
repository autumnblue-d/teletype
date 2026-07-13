// Shared i2c-follower grid view -- see kria_i2c_view.h. Split out of kria_i2c.c
// so follower output/persistence and this grid UI live in separate files. All
// bank access goes through the kria_i2c.h public API; the setters mark the bank
// dirty, so this file never touches the dirty flag directly.

#include "kria_i2c_view.h"

#include <string.h>

#include "grid_led.h"  // GRID_L0/2 ramp + grid_led_finalize
#include "kria_i2c.h"  // follower bank accessors + setters

#define KM_LB GRID_L2  // bright / on
#define KM_LD GRID_L0  // dim / off

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

// follower index at cell (x,y), or -1
static int8_t view_at(uint8_t x, uint8_t y) {
    if (y < 2 || y > 5) return -1;
    int8_t f = (x == 5) ? (y - 2) : (x == 6) ? (y - 2 + 4) : -1;
    return (f >= 0 && f < KR_I2C_FOLLOWERS) ? f : -1;
}

void kria_i2c_view_render(uint8_t* led, uint8_t vari) {
    uint8_t i;
    memset(led, 0, 128);

    for (i = 0; i < KR_I2C_FOLLOWERS; i++) {
        uint8_t cell = 5 + (i / 4) + (2 + i % 4) * 16;
        if (view_sel >= 0)
            led[cell] = (i == view_sel) ? KM_LB : KM_LD;
        else
            led[cell] = kria_i2c_follower(i)->active ? KM_LB : KM_LD;
    }
    led[112 + 5] = view_mod ? KM_LB : KM_LD;  // (5,7) config modifier

    if (view_sel >= 0) {
        i2c_follower_t* f = kria_i2c_follower(view_sel);
        for (i = 0; i < KR_NUM_TRACKS; i++)  // per-track routing (row 7)
            led[112 + i] = (f->track_en & (1 << i)) ? KM_LB : KM_LD;
        memset(led, KM_LD, 7);  // octave selector (row 0, cols 0-6)
        led[f->oct + 3] = KM_LB;
        if (f->ops->mode_ct > 1) {  // operating mode (row 0, cols 12+)
            memset(led + 12, KM_LD, f->ops->mode_ct);
            led[12 + f->active_mode] = KM_LB;
        }
    }

    grid_led_finalize(led, vari);
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
        i2c_follower_t* f = kria_i2c_follower(view_sel);
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
        }
        else if (y == 0 && f->ops->mode_ct > 1 && x >= 12 &&
                 x < 12 + f->ops->mode_ct) {
            kria_i2c_set_mode(view_sel, x - 12);
        }
        else if (y == 7 && x < KR_NUM_TRACKS) {
            kria_i2c_toggle_track(view_sel, x);
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
            }
        }
    }
}
