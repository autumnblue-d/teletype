#include "mode_persist.h"

#include <stdint.h>
#include <string.h>

#include "flash.h"          // flash_update_kria_i2c
#include "font.h"           // font_string_region_clip
#include "globals.h"        // region line[8]
#include "init_teletype.h"  // get_ticks (ms)
#include "kria_i2c.h"       // shared follower bank + i2c grid view
#include "kria_i2c_oled.h"  // MIDI-follower OLED editor
#include "region.h"         // region_fill
#include "util.h"           // itoa

// How long the "SAVED" banner stays up (ms). Long enough to read, short enough
// not to linger over the state it replaces.
#define MODE_CONFIRM_MS 900

static char confirm_msg[12];
static uint32_t confirm_until = 0;
static bool confirm_showing = false;

static bool confirm_expired(void) {
    // ms tick counter; signed diff tolerates wrap.
    return (int32_t)((uint32_t)get_ticks() - confirm_until) >= 0;
}

void mode_confirm_show(const char* msg) {
    size_t n = strlen(msg);
    if (n >= sizeof(confirm_msg)) n = sizeof(confirm_msg) - 1;
    memcpy(confirm_msg, msg, n);
    confirm_msg[n] = 0;
    confirm_until = (uint32_t)get_ticks() + MODE_CONFIRM_MS;
    confirm_showing = true;
}

bool mode_confirm_active(const char** out_msg) {
    if (!confirm_showing) return false;
    if (confirm_expired()) {
        confirm_showing = false;
        return false;
    }
    if (out_msg) *out_msg = confirm_msg;
    return true;
}

bool mode_confirm_tick(void) {
    if (confirm_showing && confirm_expired()) {
        confirm_showing = false;
        return true;  // just expired -> caller should redraw to erase it
    }
    return false;
}

bool mode_flush_i2c_if_dirty(void) {
    if (kria_i2c_take_dirty()) {
        kria_i2c_fstate_t t[KRIA_I2C_FOLLOWERS];
        kria_i2c_save(t);
        flash_update_kria_i2c(t);
        return true;
    }
    return false;
}

void mode_persist_flush_all_dirty(void) {
    // Order/short-circuit irrelevant: each wrapper self-gates on its own dirty
    // flag. Silent by design -- no confirmation banner here.
    kria_flush_if_dirty();
    earthsea_flush_if_dirty();
}

bool mode_i2c_oled_handle_key(uint8_t key, uint8_t mod_key, uint8_t is_held,
                              bool* dirty) {
    // editor doesn't own the keyboard
    if (!kria_i2c_oled_active()) return false;
    if (kria_i2c_oled_key(key, mod_key, is_held)) {
        if (!kria_i2c_oled_active())
            mode_flush_i2c_if_dirty();  // exited via <enter>
        *dirty = true;
        return true;  // key consumed
    }
    // not an editor key: leave the editor and fall through to normal handling
    kria_i2c_oled_exit();
    mode_flush_i2c_if_dirty();
    *dirty = true;
    return false;
}

bool mode_i2c_oled_render_active(void) {
    if (!kria_i2c_oled_active()) return false;
    kria_i2c_oled_render();
    return true;
}

void mode_i2c_view_grid_key(uint8_t x, uint8_t y, uint8_t z) {
    kria_i2c_view_key(x, y, z);
    if (z) {
        int8_t req = kria_i2c_view_take_oled_req();
        if (req >= 0) kria_i2c_oled_enter((uint8_t)req);
    }
}

void mode_draw_num(uint8_t ln, uint8_t x, int val, uint8_t fg) {
    char s[8];
    itoa(val, s, 10);
    font_string_region_clip(&line[ln], s, x, 0, fg, 0);
}

bool mode_screen_begin(bool* dirty, const char* app_title, uint8_t* out_mask) {
    if (!*dirty) {
        *out_mask = 0;
        return false;
    }
    *dirty = false;

    if (mode_i2c_oled_render_active()) {
        *out_mask = 0b11111111;
        return false;
    }

    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);

    const char* cmsg;
    const char* title = mode_confirm_active(&cmsg) ? cmsg : app_title;
    font_string_region_clip(&line[0], title, 0, 0, MODE_S_TITLE, 0);
    return true;
}
