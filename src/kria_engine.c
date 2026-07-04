// Kria sequencer engine -- hardware-abstract port of Ansible's Kria
// (src/ansible_grid.c). See kria_engine.h for the architecture and the
// engine/shell split. All timing (gate length, repeat spacing) is deferred to
// the shell; this file is pure sequencing logic + output vtable calls.

#include "kria_engine.h"

#include <string.h>

// ---- small helpers (mirror Ansible min/max/sum_clip) ----

static inline int imin(int a, int b) {
    return a < b ? a : b;
}
static inline int imax(int a, int b) {
    return a > b ? a : b;
}
static inline int sum_clip(int l, int r, int clip) {
    return imin(clip, imax(0, l + r));
}

static void calc_scale_index(kria_engine_t* e, uint8_t s) {
    if (!e->scale_data) return;
    kria_engine_calc_scale(e, e->scale_data[s]);
}

// ---- per-parameter position advance (Ansible kria_next_step) ----
//
// Increments the divider sub-counter; only advances the step when it reaches
// tmul. Returns true if the landed step fires (probability gate).
static bool kria_next_step(kria_engine_t* e, uint8_t t, uint8_t p) {
    kria_track_t* track = &e->cfg.p[e->cfg.pattern].t[t];
    e->rt.pos_mul[t][p]++;

    bool latch_input = false;
    if (e->cfg.sync_mode == KR_SYNC_NONE) { latch_input = true; }
    else {
        switch (track->direction) {
            case KR_DIR_FORWARD:
                if (e->rt.pos[t][p] == track->lstart[p]) latch_input = true;
                break;
            case KR_DIR_REVERSE:
                if (e->rt.pos[t][p] == track->lend[p]) latch_input = true;
                break;
            case KR_DIR_TRIANGLE:
                if (e->rt.pos[t][p] == track->lstart[p] ||
                    e->rt.pos[t][p] == track->lend[p])
                    latch_input = true;
                break;
            default: latch_input = true; break;
        }
    }

    if (e->cfg.sync_mode & KR_SYNC_TIMEDIV) {
        if (latch_input) e->rt.tmul_live[t][p] = track->tmul[p];
    }
    else { e->rt.tmul_live[t][p] = track->tmul[p]; }

    if (e->rt.pos_mul[t][p] >= e->rt.tmul_live[t][p]) {
        e->rt.pos_mul[t][p] = 0;

        switch (track->direction) {
            default:
            case KR_DIR_FORWARD:
            forward:
                if (e->rt.pos[t][p] == track->lend[p])
                    e->rt.pos[t][p] = track->lstart[p];
                else {
                    e->rt.pos[t][p]++;
                    if (e->rt.pos[t][p] > 15) e->rt.pos[t][p] = 0;
                }
                break;
            case KR_DIR_REVERSE:
            reverse:
                if (e->rt.pos[t][p] == track->lstart[p])
                    e->rt.pos[t][p] = track->lend[p];
                else {
                    e->rt.pos[t][p]--;
                    if (e->rt.pos[t][p] > 15) e->rt.pos[t][p] = 15;
                }
                break;
            case KR_DIR_TRIANGLE:
                if (e->rt.pos[t][p] == track->lend[p])
                    track->advancing[p] = false;
                if (e->rt.pos[t][p] == track->lstart[p])
                    track->advancing[p] = true;
                if (track->advancing[p])
                    goto forward;
                else
                    goto reverse;
                break;
            case KR_DIR_DRUNK:
                if (e->rnd(e->rnd_ctx) % 2)
                    goto forward;
                else
                    goto reverse;
                break;
            case KR_DIR_RANDOM: {
                uint8_t lstart = track->lstart[p];
                uint8_t lend = track->lend[p];
                uint8_t llen = track->llen[p];
                if (lend >= lstart)
                    e->rt.pos[t][p] =
                        lstart + e->rnd(e->rnd_ctx) % (lend - lstart + 1);
                else
                    e->rt.pos[t][p] =
                        (lstart + e->rnd(e->rnd_ctx) % (llen + 1)) % 16;
                break;
            }
        }

        switch (track->p[p][e->rt.pos[t][p]]) {
            case 0: return false;
            case 1: return (e->rnd(e->rnd_ctx) & 0xff) > 192;  // ~25%
            case 2: return (e->rnd(e->rnd_ctx) & 0xff) > 128;  // ~50%
            case 3:
            default: return true;
        }
    }
    return false;
}

// ---- latch the value params (Ansible clock_kria_note) ----
static void clock_kria_note(kria_engine_t* e, uint8_t t) {
    kria_track_t* track = &e->cfg.p[e->cfg.pattern].t[t];

    if (kria_next_step(e, t, KR_P_DUR)) {
        // Nominal (unscaled) gate length; the shell scales by measured clock
        // deltas to real ticks: (dur[step]+1) * (dur_mul<<2).
        e->rt.dur_unscaled[t] =
            (uint16_t)((track->dur[e->rt.pos[t][KR_P_DUR]] + 1) *
                       (track->dur_mul << 2));
    }
    if (kria_next_step(e, t, KR_P_OCT)) {
        e->rt.oct[t] = (uint8_t)sum_clip(track->octshift,
                                         track->oct[e->rt.pos[t][KR_P_OCT]], 5);
    }
    if (kria_next_step(e, t, KR_P_NOTE)) {
        e->rt.note[t] = track->note[e->rt.pos[t][KR_P_NOTE]];
    }
    if (kria_next_step(e, t, KR_P_ALTNOTE)) {
        e->rt.alt_note[t] = track->alt_note[e->rt.pos[t][KR_P_ALTNOTE]];
    }
    if (kria_next_step(e, t, KR_P_GLIDE)) {
        e->rt.glide[t] = track->glide[e->rt.pos[t][KR_P_GLIDE]];
    }
}

// ---- pitch -> CV (Ansible kria_set_note) ----
static void kria_set_note(kria_engine_t* e, uint8_t t) {
    uint8_t combined = e->rt.note[t] + e->rt.alt_note[t];
    uint8_t noteInScale = combined % 7;
    uint8_t octaveBump = combined / 7;
    int16_t semitones = (int16_t)((int)e->rt.cur_scale[noteInScale] +
                                  e->rt.scale_adj[noteInScale] +
                                  (int)((e->rt.oct[t] + octaveBump) * 12));
    e->out.cv(e->out.ctx, t, semitones);
}

// ---- per-track clock (Ansible clock_kria_track) ----
static void clock_kria_track(kria_engine_t* e, uint8_t t) {
    kria_track_t* track = &e->cfg.p[e->cfg.pattern].t[t];

    bool trNextStep = kria_next_step(e, t, KR_P_TR);
    bool isTrigger = track->tr[e->rt.pos[t][KR_P_TR]];

    if (!track->trigger_clocked) clock_kria_note(e, t);

    if (kria_next_step(e, t, KR_P_RPT)) {
        e->rt.rpt[t] = track->rpt[e->rt.pos[t][KR_P_RPT]];
        e->rt.rptBits[t] = track->rptBits[e->rt.pos[t][KR_P_RPT]];
    }

    if (trNextStep && isTrigger) {
        if (!e->rt.mutes[t]) {
            e->out.cv_slew(e->out.ctx, t, (uint16_t)(e->rt.glide[t] * 20));
            e->rt.activeRpt[t] = e->rt.rpt[t];
            e->rt.repeats[t] = (int16_t)e->rt.rpt[t] - 1;
            // (shell schedules the repeat timer from rt.repeats + dur_unscaled)

            if (e->rt.rptBits[t] & 1) {
                if (track->trigger_clocked) clock_kria_note(e, t);
                kria_set_note(e, t);
                e->out.tr(e->out.ctx, t, 1);
                e->rt.tr[t] = 1;
                // (shell schedules note-off -> kria_engine_note_off)
            }
        }
    }
}

// ---- public API ----

void kria_engine_calc_scale(kria_engine_t* e, const uint8_t intervals[8]) {
    e->rt.cur_scale[0] = intervals[0];
    for (uint8_t i = 1; i < 8; i++)
        e->rt.cur_scale[i] = e->rt.cur_scale[i - 1] + intervals[i];
}

void kria_engine_change_pattern(kria_engine_t* e, uint8_t pattern) {
    if (pattern >= KRIA_NUM_PATTERNS) return;
    e->cfg.pattern = pattern;
    e->rt.pos_reset = true;
    calc_scale_index(e, e->cfg.p[pattern].scale);
}

void kria_engine_set_mute(kria_engine_t* e, uint8_t track, uint8_t mute) {
    if (track >= KRIA_NUM_TRACKS) return;
    e->rt.mutes[track] = mute ? 1 : 0;
}

void kria_engine_set_loop_start(kria_engine_t* e, uint8_t track, uint8_t param,
                                uint8_t start) {
    if (track >= KRIA_NUM_TRACKS || param >= KRIA_NUM_PARAMS) return;
    kria_track_t* t = &e->cfg.p[e->cfg.pattern].t[track];
    t->lstart[param] = start & 0x0f;
    int end = (int)t->lstart[param] + (int)t->llen[param] - 1;
    if (end > 15) {
        t->lend[param] = (uint8_t)(end - 16);
        t->lswap[param] = 1;
    }
    else {
        t->lend[param] = (uint8_t)end;
        t->lswap[param] = 0;
    }
}

void kria_engine_set_loop_len(kria_engine_t* e, uint8_t track, uint8_t param,
                              uint8_t len) {
    if (track >= KRIA_NUM_TRACKS || param >= KRIA_NUM_PARAMS) return;
    if (len < 1) len = 1;
    if (len > 16) len = 16;
    kria_track_t* t = &e->cfg.p[e->cfg.pattern].t[track];
    t->llen[param] = len;
    int end = (int)t->lstart[param] + (int)len - 1;
    if (end > 15) {
        t->lend[param] = (uint8_t)(end - 16);
        t->lswap[param] = 1;
    }
    else {
        t->lend[param] = (uint8_t)end;
        t->lswap[param] = 0;
    }
}

void kria_engine_reset(kria_engine_t* e) {
    for (uint8_t t = 0; t < KRIA_NUM_TRACKS; t++) {
        kria_track_t* track = &e->cfg.p[e->cfg.pattern].t[t];
        for (uint8_t p = 0; p < KRIA_NUM_PARAMS; p++) {
            // pos = lend and pos_mul = tmul so the next clock advances straight
            // to lstart (Ansible pos_reset semantics).
            e->rt.pos[t][p] = track->lend[p];
            e->rt.pos_mul[t][p] = track->tmul[p];
            e->rt.tmul_live[t][p] = track->tmul[p];
        }
        e->rt.note[t] = 0;
        e->rt.oct[t] = 0;
        e->rt.alt_note[t] = 0;
        e->rt.glide[t] = 0;
        e->rt.dur_unscaled[t] = 0;
        e->rt.rpt[t] = 1;
        e->rt.rptBits[t] = 1;
        e->rt.activeRpt[t] = 0;
        e->rt.repeats[t] = 0;
        e->rt.tr[t] = 0;
    }
    e->rt.clock_count = 0;
    e->rt.cue_count = 0;
    e->rt.cue_sub_count = 0;
    e->rt.meta_pos = e->cfg.meta_start;
    e->rt.meta_count = 0;
    e->rt.meta_next = 0;
    e->rt.cue_pat_next = 0;
    e->rt.pos_reset = false;
    e->rt.meta_reset = false;
}

void kria_engine_init(kria_engine_t* e, const kria_output_t* out,
                      uint32_t (*rnd)(void* ctx), void* rnd_ctx,
                      const uint8_t (*scale_data)[8]) {
    memset(&e->rt, 0, sizeof(e->rt));
    e->out = *out;
    e->rnd = rnd;
    e->rnd_ctx = rnd_ctx;
    e->scale_data = scale_data;
    calc_scale_index(e, e->cfg.p[e->cfg.pattern].scale);
    kria_engine_reset(e);
}

void kria_engine_clock(kria_engine_t* e, uint8_t phase) {
    if (!phase) return;  // Kria acts only on the rising edge

    e->rt.clock_count++;
    e->rt.cue_sub_count++;

    if (e->rt.cue_sub_count >= e->cfg.cue_div + 1) {
        e->rt.cue_sub_count = 0;
        e->rt.cue_count++;
        if (e->rt.cue_count >= e->cfg.cue_steps + 1) {
            e->rt.cue_count = 0;
            if (e->cfg.meta) {
                e->rt.meta_count++;
                if (e->rt.meta_count > e->cfg.meta_steps[e->rt.meta_pos]) {
                    if (e->rt.meta_next)
                        e->rt.meta_pos = e->rt.meta_next - 1;
                    else if (e->rt.meta_pos == e->cfg.meta_end)
                        e->rt.meta_pos = e->cfg.meta_start;
                    else
                        e->rt.meta_pos++;
                    kria_engine_change_pattern(e,
                                               e->cfg.meta_pat[e->rt.meta_pos]);
                    e->rt.meta_next = 0;
                    e->rt.meta_count = 0;
                }
            }
            else if (e->rt.cue_pat_next) {
                kria_engine_change_pattern(e, e->rt.cue_pat_next - 1);
                e->rt.cue_pat_next = 0;
            }
        }
    }

    if (e->cfg.meta && e->rt.meta_reset) {
        e->rt.meta_pos = e->cfg.meta_start;
        kria_engine_change_pattern(e, e->cfg.meta_pat[e->rt.meta_pos]);
        e->rt.meta_next = 0;
        e->rt.meta_count = 0;
        e->rt.meta_reset = false;
    }

    if (e->rt.pos_reset) {
        e->rt.clock_count = 0;
        for (uint8_t i1 = 0; i1 < KRIA_NUM_TRACKS; i1++)
            for (uint8_t i2 = 0; i2 < KRIA_NUM_PARAMS; i2++) {
                e->rt.pos[i1][i2] = e->cfg.p[e->cfg.pattern].t[i1].lend[i2];
                e->rt.pos_mul[i1][i2] = e->cfg.p[e->cfg.pattern].t[i1].tmul[i2];
            }
        e->rt.cue_count = 0;
        e->rt.cue_sub_count = 0;
        e->rt.pos_reset = false;
    }

    for (uint8_t i = 0; i < KRIA_NUM_TRACKS; i++) {
        if (!e->cfg.p[e->cfg.pattern].t[i].tt_clocked) clock_kria_track(e, i);
    }
}

void kria_engine_clock_track(kria_engine_t* e, uint8_t track) {
    if (track >= KRIA_NUM_TRACKS) return;
    clock_kria_track(e, track);
}

void kria_engine_note_off(kria_engine_t* e, uint8_t track) {
    kria_track_t* t = &e->cfg.p[e->cfg.pattern].t[track];
    // dur_tie_mode: hold the gate on a max-duration step with no repeats left.
    if (e->cfg.dur_tie_mode && t->dur[e->rt.pos[track][KR_P_DUR]] == 5 &&
        e->rt.repeats[track] <= 0)
        return;
    e->out.tr(e->out.ctx, track, 0);
    e->rt.tr[track] = 0;
}

void kria_engine_repeat(kria_engine_t* e, uint8_t track) {
    kria_track_t* t = &e->cfg.p[e->cfg.pattern].t[track];
    uint8_t bit = (uint8_t)(e->rt.activeRpt[track] - e->rt.repeats[track]);
    e->rt.repeats[track]--;
    if (t->rptBits[e->rt.pos[track][KR_P_RPT]] & (1 << bit)) {
        if (t->trigger_clocked) clock_kria_note(e, track);
        kria_set_note(e, track);
        e->out.tr(e->out.ctx, track, 1);
        e->rt.tr[track] = 1;
    }
}

void kria_engine_set_defaults(kria_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));

    kria_track_t t0;
    memset(&t0, 0, sizeof(t0));
    // step arrays
    memset(t0.rpt, 1, 16);
    memset(t0.rptBits, 1, 16);
    memset(t0.p, 3, sizeof(t0.p));  // all probabilities 3 (100%)
    // scalars
    t0.dur_mul = 4;
    t0.direction = KR_DIR_FORWARD;
    memset(t0.advancing, 1, KRIA_NUM_PARAMS);
    memset(t0.lend, 5, KRIA_NUM_PARAMS);
    memset(t0.llen, 6, KRIA_NUM_PARAMS);
    memset(t0.tmul, 1, KRIA_NUM_PARAMS);
    // tr/oct/note/dur/alt_note/glide/lstart/lswap/octshift/tt/trigger already 0

    for (uint8_t p = 0; p < KRIA_NUM_PATTERNS; p++) {
        for (uint8_t tr = 0; tr < KRIA_NUM_TRACKS; tr++) cfg->p[p].t[tr] = t0;
        cfg->p[p].scale = 0;
    }

    cfg->pattern = 0;
    cfg->meta_start = 0;
    cfg->meta_end = 3;
    cfg->meta_len = 4;
    cfg->meta_lswap = 0;
    memset(cfg->meta_pat, 0, 64);
    memset(cfg->meta_steps, 7, 64);
    memset(cfg->glyph, 0, 8);

    cfg->sync_mode = KR_SYNC_NONE;
    cfg->cue_div = 0;
    cfg->cue_steps = 3;
    cfg->meta = 0;
    cfg->meta_reset_all = 0;
    cfg->dur_tie_mode = 0;
    cfg->clock_period = 60;
}

bool kria_engine_config_valid(const kria_config_t* cfg) {
    if (cfg->pattern >= KRIA_NUM_PATTERNS) return false;

    for (uint8_t p = 0; p < KRIA_NUM_PATTERNS; p++) {
        if (cfg->p[p].scale >= 16) return false;  // scale bank has 16 slots
        for (uint8_t tr = 0; tr < KRIA_NUM_TRACKS; tr++) {
            const kria_track_t* t = &cfg->p[p].t[tr];
            if (t->direction > KR_DIR_RANDOM) return false;
            if (t->dur_mul == 0) return false;
            for (uint8_t i = 0; i < KRIA_NUM_PARAMS; i++) {
                if (t->lstart[i] > 15 || t->lend[i] > 15) return false;
                if (t->llen[i] > 16) return false;
                if (t->tmul[i] == 0) return false;  // divider must be >= 1
            }
            for (uint8_t s = 0; s < 16; s++)
                if (t->rpt[s] == 0)
                    return false;  // downstream dur/rpt div guard
        }
    }

    for (uint8_t i = 0; i < 64; i++)
        if (cfg->meta_pat[i] >= KRIA_NUM_PATTERNS) return false;
    if (cfg->meta_start >= 64 || cfg->meta_end >= 64) return false;

    return true;
}
