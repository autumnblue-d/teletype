// Meadowphysics grid surface -- see meadowphysics_grid.h.
// Ported from Ansible handler_MPGridKey (NORMAL branch) and refresh_mp
// (ansible/src/ansible_grid.c). Globals -> engine/grid-state fields;
// monomeLedBuffer -> the passed-in `led`.

#include "meadowphysics_grid.h"

#include <string.h>  // memset

// LED brightness levels (Ansible L0/L1/L2).
#define MP_LED_DIM 4
#define MP_LED_MED 8
#define MP_LED_BRI 12

#define MP_GRID_COLS 16

// Rule glyphs (8 rules x 8 rows of an 8-wide bitmap), from Ansible `sign`.
static const uint8_t mp_rule_sign[8][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },              // o  none
    { 0, 24, 24, 126, 126, 24, 24, 0 },      // +  inc
    { 0, 0, 0, 126, 126, 0, 0, 0 },          // -  dec
    { 0, 96, 96, 126, 126, 96, 96, 0 },      // >  max
    { 0, 6, 6, 126, 126, 6, 6, 0 },          // <  min
    { 0, 102, 102, 24, 24, 102, 102, 0 },    // *  rnd
    { 0, 120, 120, 102, 102, 30, 30, 0 },    // <> pole
    { 0, 126, 126, 102, 102, 126, 126, 0 },  // [] stop
};

void mp_grid_state_init(mp_grid_state_t* g) {
    g->edit_mode = MP_GRID_POSITIONS;
    g->edit_row = 0;
    g->kcount = 0;
    for (uint8_t i = 0; i < MP_ROWS; i++) g->scount[i] = 0;
}

void mp_grid_process_key(mp_engine_t* e, mp_grid_state_t* g, uint8_t x,
                         uint8_t y, uint8_t z) {
    if (y >= MP_ROWS) return;

    mp_config_t* m = &e->cfg;
    mp_runtime_t* r = &e->rt;

    // column 0: hold to enter speed sub-mode; also selects edit_row
    if (x == 0) {
        g->kcount += (z << 1) - 1;
        if (g->kcount < 0) g->kcount = 0;

        if (g->kcount == 1 && z == 1)
            g->edit_mode = MP_GRID_SPEED;
        else if (g->kcount == 0) {
            g->edit_mode = MP_GRID_POSITIONS;
            g->scount[y] = 0;
        }

        if (z == 1 && g->edit_mode == MP_GRID_SPEED) g->edit_row = y;
    }
    // column 1: while in speed sub-mode, hold to enter rules sub-mode
    else if (x == 1 && g->edit_mode != MP_GRID_POSITIONS) {
        if (g->edit_mode == MP_GRID_SPEED && z == 1) {
            g->edit_mode = MP_GRID_RULES;
            g->edit_row = y;
        }
        else if (g->edit_mode == MP_GRID_RULES && z == 0)
            g->edit_mode = MP_GRID_SPEED;
    }
    // positions: set count (1st press) / range (2nd press) / manual push
    else if (g->edit_mode == MP_GRID_POSITIONS) {
        g->scount[y] += (z << 1) - 1;
        if (g->scount[y] < 0) g->scount[y] = 0;

        if (z == 1 && g->scount[y] == 1) {
            r->position[y] = x;
            m->count[y] = x;
            m->min[y] = x;
            m->max[y] = x;
            r->tick[y] = m->speed[y];
            if (m->sound) r->pushed[y] = 1;
        }
        else if (z == 1 && g->scount[y] == 2) {
            if (x < m->count[y]) {
                m->min[y] = x;
                m->max[y] = m->count[y];
            }
            else {
                m->max[y] = x;
                m->min[y] = m->count[y];
            }
        }
    }
    // speed + trigger/toggle/sync/sound
    else if (g->edit_mode == MP_GRID_SPEED) {
        g->scount[y] += (z << 1) - 1;
        if (g->scount[y] < 0) g->scount[y] = 0;

        if (z == 1) {
            if (x > 7) {  // right half: speed value (1st press) / range (2nd)
                if (g->scount[y] == 1) {
                    m->smin[y] = x - 8;
                    m->smax[y] = x - 8;
                    m->speed[y] = x - 8;
                    r->tick[y] = m->speed[y];
                }
                else if (g->scount[y] == 2) {
                    if (x - 8 < m->smin[y]) {
                        m->smax[y] = m->smin[y];
                        m->smin[y] = x - 8;
                    }
                    else
                        m->smax[y] = x - 8;
                }
            }
            else if (x == 5) {  // toggle bit for edit_row -> row y
                m->toggle[g->edit_row] ^= 1 << y;
                m->trigger[g->edit_row] &= ~(1 << y);
            }
            else if (x == 6) {  // trigger bit
                m->trigger[g->edit_row] ^= 1 << y;
                m->toggle[g->edit_row] &= ~(1 << y);
            }
            else if (x == 4) {  // sound (manual-play) toggle
                m->sound ^= 1;
            }
            else if (x == 2) {  // stop/start row y
                if (r->position[y] == -1)
                    r->position[y] = m->count[y];
                else
                    r->position[y] = -1;
            }
            else if (x == 3) {  // sync bit
                m->sync[g->edit_row] ^= (1 << y);
            }
        }
    }
    // rules: destination + target (cols 4-6), rule select (cols 7+)
    else if (g->edit_mode == MP_GRID_RULES && z == 1) {
        if (x > 3 && x < 7) {
            m->rule_dests[g->edit_row] = y;
            m->rule_dest_targets[g->edit_row] = x - 3;
        }
        else if (x > 6) { m->rules[g->edit_row] = y; }
    }
}

// Bounded LED write: row masked to 0..MP_ROWS-1, column dropped if >= 16.
// Guarantees no out-of-bounds write to the 128-byte buffer even if a config
// value (used as a row or column here) is out of range.
static inline void led_set(uint8_t* led, uint8_t row, uint8_t col, uint8_t v) {
    if (col < MP_GRID_COLS) led[(row & (MP_ROWS - 1)) * MP_GRID_COLS + col] = v;
}

void mp_grid_refresh(mp_engine_t* e, mp_grid_state_t* g, uint8_t* led,
                     uint8_t vari) {
    mp_config_t* m = &e->cfg;
    mp_runtime_t* r = &e->rt;
    uint8_t er = g->edit_row & (MP_ROWS - 1);

    memset(led, 0, MP_ROWS * MP_GRID_COLS);

    if (g->edit_mode == MP_GRID_POSITIONS) {
        for (uint8_t i = 0; i < MP_ROWS; i++) {
            for (uint8_t c = m->min[i]; c <= m->max[i] && c < MP_GRID_COLS; c++)
                led_set(led, i, c, MP_LED_DIM);
            led_set(led, i, m->count[i], MP_LED_MED);
            if (r->position[i] >= 0)
                led_set(led, i, r->position[i], MP_LED_BRI);
        }
    }
    else if (g->edit_mode == MP_GRID_SPEED) {
        for (uint8_t i = 0; i < MP_ROWS; i++) {
            if (r->position[i] >= 0)
                led_set(led, i, r->position[i], MP_LED_DIM);
            if (r->position[i] != -1) led_set(led, i, 2, 2);

            for (uint8_t s = m->smin[i];
                 s <= m->smax[i] && s + 8 < MP_GRID_COLS; s++)
                led_set(led, i, s + 8, MP_LED_DIM);
            led_set(led, i, m->speed[i] + 8, MP_LED_MED);

            if (m->sound) led_set(led, i, 4, 2);

            led_set(led, i, 5,
                    (m->toggle[er] & (1 << i)) ? MP_LED_BRI : MP_LED_DIM);
            led_set(led, i, 6,
                    (m->trigger[er] & (1 << i)) ? MP_LED_BRI : MP_LED_DIM);
            led_set(led, i, 3,
                    (m->sync[er] & (1 << i)) ? MP_LED_MED : MP_LED_DIM);
        }
        led_set(led, er, 0, MP_LED_BRI);
    }
    else {  // MP_GRID_RULES
        for (uint8_t i = 0; i < MP_ROWS; i++)
            if (r->position[i] >= 0)
                led_set(led, i, r->position[i], MP_LED_DIM);

        led_set(led, er, 0, MP_LED_MED);
        led_set(led, er, 1, MP_LED_MED);

        uint8_t dest = m->rule_dests[er];
        uint8_t tgt = m->rule_dest_targets[er];
        led_set(led, dest, 4, (tgt == 2) ? MP_LED_DIM : MP_LED_BRI);
        led_set(led, dest, 5, (tgt == 1) ? MP_LED_DIM : MP_LED_BRI);
        led_set(led, dest, 6, MP_LED_DIM);

        for (uint8_t c = 8; c < 16; c++)
            led_set(led, m->rules[er], c, MP_LED_DIM);

        for (uint8_t i = 0; i < MP_ROWS; i++) {
            uint8_t bits = mp_rule_sign[m->rules[er] & 0x7][i];
            for (uint8_t b = 0; b < 8; b++)
                if (bits & (1 << b)) led_set(led, i, 8 + b, MP_LED_BRI);
        }
    }

    if (!vari)  // mono grid: any lit cell to full brightness (B5 fallback)
        for (uint16_t i = 0; i < MP_ROWS * MP_GRID_COLS; i++)
            if (led[i]) led[i] = 15;
}

// ---- config-view scale editor (Ansible view_config / refresh_mp_config) ----

bool mp_grid_scale_key(mp_engine_t* e, uint8_t (*bank)[8], uint8_t x, uint8_t y,
                       uint8_t z) {
    if (!z) return false;
    if (y >= 6 && x < 8) {  // slot select (rows 6-7 x cols 0-7 = 16 slots)
        uint8_t slot = (y - 6) * 8 + x;
        if (slot < MP_SCALE_SLOTS) e->cfg.scale = slot;
        return false;  // slot change only; no bank edit
    }
    if (x >= 8) {  // interval edit: row y -> degree 7-y, value = x-8 (0-7)
        bank[e->cfg.scale][7 - y] = x - 8;
        return true;  // bank edited -> persist
    }
    return false;
}

void mp_grid_scale_refresh(mp_engine_t* e, uint8_t (*bank)[8], uint8_t* led,
                           uint8_t vari) {
    uint8_t slot = e->cfg.scale;
    memset(led, 0, MP_ROWS * MP_GRID_COLS);

    // interval editor: right half, one row per degree (row y = degree 7-y),
    // lit column = that degree's interval; dim marker at col 8 as a baseline.
    for (uint8_t d = 0; d < 8; d++) {
        uint8_t yy = 7 - d;
        led_set(led, yy, 8, MP_LED_DIM);
        led_set(led, yy, 8 + bank[slot][d], MP_LED_MED);
    }
    // slot select: rows 6-7 x cols 0-7 (16 slots); current slot bright
    for (uint8_t s = 0; s < MP_SCALE_SLOTS; s++)
        led_set(led, 6 + (s >> 3), s & 7, 2);
    led_set(led, 6 + (slot >> 3), slot & 7, MP_LED_BRI);

    if (!vari)
        for (uint16_t i = 0; i < MP_ROWS * MP_GRID_COLS; i++)
            if (led[i]) led[i] = 15;
}
