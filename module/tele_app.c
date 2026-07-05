#include "tele_app.h"

#include "earthsea_mode.h"
#include "kria_mode.h"
#include "meadowphysics_mode.h"

const tele_app_t tele_apps[TELE_APP_COUNT] = {
    { meadowphysics_owns_grid, meadowphysics_grid_key,
      meadowphysics_grid_render, meadowphysics_suppresses_output },
    { kria_owns_grid, kria_grid_key, kria_grid_render, kria_suppresses_output },
    { es_owns_grid, es_grid_key, es_grid_render, es_suppresses_output },
};
