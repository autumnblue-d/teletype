#ifndef _TELE_APP_H_
#define _TELE_APP_H_

#include <stdbool.h>
#include <stdint.h>

// Registry of the ported grid apps (Meadowphysics, Kria, Earthsea) over the
// event seams whose signatures are uniform across all three, so the module can
// iterate them instead of hand-enumerating each app at every dispatch site
// (grid key, grid render, output suppression). The non-uniform seams -- per-app
// clock/service events, trigger inputs, OLED refresh, mode enter/exit -- stay
// explicit in handler_AppCustom / handler_Trigger / the mode switch, since they
// differ in shape per app and also cover the built-in (non-app) modes.
typedef struct {
    bool (*owns_grid)(void);  // app is driving the grid right now
    void (*grid_key)(uint8_t x, uint8_t y, uint8_t z);  // grid press/release
    void (*grid_render)(void);              // render into the shared LED buffer
    bool (*suppresses_output)(uint8_t ch);  // app owns CV/TR channel ch
} tele_app_t;

#define TELE_APP_COUNT 3

// Order matches the historical if-cascade (MP, Kria, Earthsea). At most one app
// owns the grid at a time; suppression is queried for every app.
extern const tele_app_t tele_apps[TELE_APP_COUNT];

#endif
