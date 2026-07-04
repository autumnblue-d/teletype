#include "mode_persist.h"

#include <stdint.h>
#include <string.h>

#include "init_teletype.h"  // get_ticks (ms)

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

void mode_persist_flush_all_dirty(void) {
    // Order/short-circuit irrelevant: each wrapper self-gates on its own dirty
    // flag. Silent by design -- no confirmation banner here.
    kria_flush_if_dirty();
    earthsea_flush_if_dirty();
}
