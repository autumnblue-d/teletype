// Meadowphysics sequencer engine -- see meadowphysics_engine.h.
// Ported from Ansible src/ansible_grid.c (clock_mp, mp_note_on/off,
// get_note_slot, calc_scale, default_mp). Logic is preserved verbatim; the only
// changes are: bare globals -> mp_engine_t fields (m.X -> e->cfg.X, position ->
// e->rt.position, etc.), hardware calls -> e->out vtable, and rnd() ->
// injected.

#include "meadowphysics_engine.h"

#include <stdlib.h>  // abs

// ---- output helpers (set_tr/clr_tr/set_cv_note/dac_set_value) ----

static inline void out_tr(mp_engine_t* e, uint8_t ch, uint8_t on) {
    if (e->out.tr) e->out.tr(e->out.ctx, ch, on);
}
static inline void out_cv(mp_engine_t* e, uint8_t ch, int16_t note) {
    if (e->out.cv) e->out.cv(e->out.ctx, ch, note);
}
static inline void out_cv_gate(mp_engine_t* e, uint8_t ch, uint8_t on) {
    if (e->out.cv_gate) e->out.cv_gate(e->out.ctx, ch, on);
}

// ---- voice allocation (Ansible get_note_slot) ----

static uint8_t get_note_slot(mp_engine_t* e, uint8_t v) {
    int8_t w = -1;

    for (int i1 = 0; i1 < v; i1++) e->rt.note_age[i1]++;

    // find empty
    for (int i1 = 0; i1 < v; i1++)
        if (e->rt.note_now[i1] == -1) {
            w = i1;
            break;
        }

    if (w == -1) {  // steal oldest
        w = 0;
        for (int i1 = 1; i1 < v; i1++)
            if (e->rt.note_age[w] < e->rt.note_age[i1]) w = i1;
    }

    e->rt.note_age[w] = 1;
    return w;
}

// ---- note on/off (Ansible mp_note_on / mp_note_off) ----

static void mp_note_on(mp_engine_t* e, uint8_t n) {
    uint8_t w;
    switch (e->cfg.voice_mode) {
        case MP_8T:
            if (n < 4)
                out_tr(e, n, 1);
            else
                out_cv_gate(e, n - 4, 1);
            break;
        case MP_1V:
            if (e->rt.mp_clock_count < 1) {
                e->rt.mp_clock_count++;
                e->rt.note_now[0] = n;
                out_cv(e, 0,
                       (int16_t)((int)e->rt.cur_scale[7 - n] +
                                 e->rt.scale_adj[7 - n]));
                out_tr(e, 0, 1);
            }
            break;
        case MP_2V:
            if (e->rt.mp_clock_count < 2) {
                e->rt.mp_clock_count++;
                w = get_note_slot(e, 2);
                e->rt.note_now[w] = n;
                out_cv(e, w,
                       (int16_t)((int)e->rt.cur_scale[7 - n] +
                                 e->rt.scale_adj[7 - n]));
                out_tr(e, w, 1);
            }
            break;
        case MP_4V:
            if (e->rt.mp_clock_count < 4) {
                e->rt.mp_clock_count++;
                w = get_note_slot(e, 4);
                e->rt.note_now[w] = n;
                out_cv(e, w,
                       (int16_t)((int)e->rt.cur_scale[7 - n] +
                                 e->rt.scale_adj[7 - n]));
                out_tr(e, w, 1);
            }
            break;
        default: break;
    }
}

static void mp_note_off(mp_engine_t* e, uint8_t n) {
    switch (e->cfg.voice_mode) {
        case MP_8T:
            if (n < 4)
                out_tr(e, n, 0);
            else
                out_cv_gate(e, n - 4, 0);
            break;
        case MP_1V:
            if (e->rt.note_now[0] == n) {
                e->rt.note_now[0] = -1;
                out_tr(e, 0, 0);
            }
            break;
        case MP_2V:
            for (int i1 = 0; i1 < 2; i1++) {
                if (e->rt.note_now[i1] == n) {
                    e->rt.note_now[i1] = -1;
                    out_tr(e, i1, 0);
                }
            }
            break;
        case MP_4V:
            for (int i1 = 0; i1 < 4; i1++) {
                if (e->rt.note_now[i1] == n) {
                    e->rt.note_now[i1] = -1;
                    out_tr(e, i1, 0);
                }
            }
            break;
        default: break;
    }
}

// ---- clock (Ansible clock_mp) ----

void mp_engine_clock(mp_engine_t* e, uint8_t phase) {
    uint8_t i;
    mp_config_t* m = &e->cfg;
    mp_runtime_t* r = &e->rt;

    r->mp_clock_count = 0;

    if (phase) {
        for (i = 0; i < 8; i++) r->pstate[i] = r->state[i];

        for (i = 0; i < 8; i++) {
            if (r->pushed[i]) {
                for (int n = 0; n < 8; n++) {
                    if (m->sync[i] & (1 << n)) { r->reset[n] = 1; }

                    if (m->trigger[i] & (1 << n)) {
                        r->state[n] = 1;
                        r->clear[n] = 1;
                    }
                    else if (m->toggle[i] & (1 << n)) { r->state[n] ^= 1; }
                }
                r->pushed[i] = 0;
            }

            if (r->tick[i] == 0) {
                r->tick[i] = m->speed[i];
                if (r->position[i] == 0) {
                    // RULES
                    if (m->rules[i] == MP_RULE_INC) {  // inc
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT) {
                            m->count[m->rule_dests[i]]++;
                            if (m->count[m->rule_dests[i]] >
                                m->max[m->rule_dests[i]]) {
                                m->count[m->rule_dests[i]] =
                                    m->min[m->rule_dests[i]];
                            }
                        }
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED) {
                            m->speed[m->rule_dests[i]]++;
                            if (m->speed[m->rule_dests[i]] >
                                m->smax[m->rule_dests[i]]) {
                                m->speed[m->rule_dests[i]] =
                                    m->smin[m->rule_dests[i]];
                            }
                        }
                    }
                    else if (m->rules[i] == MP_RULE_DEC) {  // dec
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT) {
                            m->count[m->rule_dests[i]]--;
                            if (m->count[m->rule_dests[i]] <
                                m->min[m->rule_dests[i]]) {
                                m->count[m->rule_dests[i]] =
                                    m->max[m->rule_dests[i]];
                            }
                        }
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED) {
                            m->speed[m->rule_dests[i]]--;
                            if (m->speed[m->rule_dests[i]] <
                                m->smin[m->rule_dests[i]]) {
                                m->speed[m->rule_dests[i]] =
                                    m->smax[m->rule_dests[i]];
                            }
                        }
                    }
                    else if (m->rules[i] == MP_RULE_MAX) {  // max
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT)
                            m->count[m->rule_dests[i]] =
                                m->max[m->rule_dests[i]];
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED)
                            m->speed[m->rule_dests[i]] =
                                m->smax[m->rule_dests[i]];
                    }
                    else if (m->rules[i] == MP_RULE_MIN) {  // min
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT)
                            m->count[m->rule_dests[i]] =
                                m->min[m->rule_dests[i]];
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED)
                            m->speed[m->rule_dests[i]] =
                                m->smin[m->rule_dests[i]];
                    }
                    else if (m->rules[i] == MP_RULE_RND) {  // rnd
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT)
                            m->count[m->rule_dests[i]] =
                                (e->rnd(e->rnd_ctx) %
                                 (m->max[m->rule_dests[i]] -
                                  m->min[m->rule_dests[i]] + 1)) +
                                m->min[m->rule_dests[i]];
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED)
                            m->speed[m->rule_dests[i]] =
                                (e->rnd(e->rnd_ctx) %
                                 (m->smax[m->rule_dests[i]] -
                                  m->smin[m->rule_dests[i]] + 1)) +
                                m->smin[m->rule_dests[i]];
                    }
                    else if (m->rules[i] == MP_RULE_POLE) {  // pole
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT) {
                            if (abs(m->count[m->rule_dests[i]] -
                                    m->min[m->rule_dests[i]]) <
                                abs(m->count[m->rule_dests[i]] -
                                    m->max[m->rule_dests[i]])) {
                                m->count[m->rule_dests[i]] =
                                    m->max[m->rule_dests[i]];
                            }
                            else {
                                m->count[m->rule_dests[i]] =
                                    m->min[m->rule_dests[i]];
                            }
                        }
                        if (m->rule_dest_targets[i] & MP_TARGET_SPEED) {
                            if (abs(m->speed[m->rule_dests[i]] -
                                    m->smin[m->rule_dests[i]]) <
                                abs(m->speed[m->rule_dests[i]] -
                                    m->smax[m->rule_dests[i]])) {
                                m->speed[m->rule_dests[i]] =
                                    m->smax[m->rule_dests[i]];
                            }
                            else {
                                m->speed[m->rule_dests[i]] =
                                    m->smin[m->rule_dests[i]];
                            }
                        }
                    }
                    else if (m->rules[i] == MP_RULE_STOP) {  // stop
                        if (m->rule_dest_targets[i] & MP_TARGET_COUNT)
                            r->position[m->rule_dests[i]] = -1;
                    }

                    r->position[i]--;

                    for (int n = 0; n < 8; n++) {
                        if (m->sync[i] & (1 << n)) { r->reset[n] = 1; }

                        if (m->trigger[i] & (1 << n)) {
                            r->state[n] = 1;
                            r->clear[n] = 1;
                        }
                        else if (m->toggle[i] & (1 << n)) { r->state[n] ^= 1; }
                    }
                }
                else if (r->position[i] > 0)
                    r->position[i]--;
            }
            else
                r->tick[i]--;
        }

        for (i = 0; i < 8; i++) {
            if (r->reset[i]) {
                r->position[i] = m->count[i];
                r->tick[i] = m->speed[i];
                r->reset[i] = 0;
            }
        }

        for (i = 0; i < 8; i++)
            if (r->state[i] && !r->pstate[i])
                mp_note_on(e, i);
            else if (!r->state[i] && r->pstate[i])
                mp_note_off(e, i);
    }
    else {
        for (i = 0; i < 8; i++) {
            if (r->clear[i]) {
                mp_note_off(e, i);
                r->state[i] = 0;
            }
            r->clear[i] = 0;
        }
    }
}

// ---- scale (Ansible calc_scale) ----

void mp_engine_calc_scale(mp_engine_t* e, const uint8_t intervals[8]) {
    e->rt.cur_scale[0] = intervals[0];
    for (uint8_t i1 = 1; i1 < 8; i1++)
        e->rt.cur_scale[i1] = e->rt.cur_scale[i1 - 1] + intervals[i1];
}

// ---- setup / transport ----

void mp_engine_set_defaults(mp_config_t* cfg) {
    // Ansible default_mp(): ascending counters, each row triggers+syncs itself.
    for (uint8_t i = 0; i < MP_ROWS; i++) {
        cfg->count[i] = 7 + i;  // 7..14 -- assumes a 16-wide grid
        cfg->speed[i] = 0;
        cfg->min[i] = 7 + i;
        cfg->max[i] = 7 + i;
        cfg->trigger[i] = (1 << i);
        cfg->toggle[i] = 0;
        cfg->rules[i] = MP_RULE_INC;
        cfg->rule_dests[i] = i;
        cfg->sync[i] = (1 << i);
        cfg->rule_dest_targets[i] = 3;  // count + speed
        cfg->smin[i] = 0;
        cfg->smax[i] = 0;
    }
    cfg->scale = 0;
}

void mp_engine_reset(mp_engine_t* e) {
    for (uint8_t i = 0; i < MP_ROWS; i++) {
        e->rt.position[i] = e->cfg.count[i];
        e->rt.tick[i] = 0;
        e->rt.pushed[i] = 0;
        e->rt.reset[i] = 0;
        e->rt.state[i] = 0;
        e->rt.pstate[i] = 0;
        e->rt.clear[i] = 0;
    }
    e->rt.mp_clock_count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        e->rt.note_now[i] = -1;
        e->rt.note_age[i] = 0;
    }
}

void mp_engine_reset_row(mp_engine_t* e, uint8_t row) {
    if (row >= MP_ROWS) return;
    e->rt.position[row] = e->cfg.count[row];
    e->rt.tick[row] = e->cfg.speed[row];
}

void mp_engine_stop(mp_engine_t* e) {
    for (uint8_t i = 0; i < MP_ROWS; i++) e->rt.position[i] = -1;
}

void mp_engine_stop_row(mp_engine_t* e, uint8_t row) {
    if (row >= MP_ROWS) return;
    e->rt.position[row] = -1;
}

void mp_engine_push(mp_engine_t* e, uint8_t row) {
    if (row >= MP_ROWS) return;
    e->rt.pushed[row] = 1;
}

void mp_engine_init(mp_engine_t* e, const mp_output_t* out,
                    uint32_t (*rnd)(void* ctx), void* rnd_ctx) {
    for (uint8_t i = 0; i < 8; i++) {
        e->rt.cur_scale[i] = 0;
        e->rt.scale_adj[i] = 0;
    }
    e->out = *out;
    e->rnd = rnd;
    e->rnd_ctx = rnd_ctx;
    e->cfg.voice_mode = MP_1V;
    e->cfg.sound = 0;
    mp_engine_set_defaults(&e->cfg);
    mp_engine_reset(e);
}
