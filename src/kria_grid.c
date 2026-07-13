// Kria grid surface -- see kria_grid.h. Ported from refresh_kria_view +
// handler_KriaGridKey (ansible/src/ansible_grid.c). Globals -> engine (cfg/rt)
// + grid-state fields; monomeLedBuffer -> the passed-in `led`.
//
// Deviations (no hardware to tune against; all documented):
//  - primary 16x8 view only (no 256 second view).
//  - plain pattern select acts on release (Ansible fast-press release), so the
//    long-press pattern-copy gesture is ported: hold a slot to copy the playing
//    pattern into it, then switch (kria_grid_pattern_hold_fire, shell-timed).
//    The mRpt long-press reset is not ported (its row is now the decrement row).
//  - the mRpt modLoop "vertical range" gesture and the meta-slot loop gesture
//    fall back to the standard loop-range gesture / no-op (marked TODO).

#include "kria_grid.h"

#include <string.h>

#include "grid_led.h"  // GRID_L0/1/2 ramp + grid_led_finalize
#include "int_math.h"  // imin/imax/sum_clip

// Brightness levels (Ansible L0/L1/L2) -- shared ramp, local aliases.
#define L0 GRID_L0
#define L1 GRID_L1
#define L2 GRID_L2

// Row base offsets.
#define R0 0
#define R1 16
#define R2 32
#define R5 80
#define R6 96
#define R7 112

// Effective edit pattern: when meta-locked, the stored edit_pattern; otherwise
// it follows the playing pattern (Ansible's edit_pattern = k.pattern default).
static uint8_t edit_pat(kria_engine_t* e, kria_grid_state_t* g) {
    return g->meta_lock ? g->edit_pattern : e->cfg.pattern;
}
// Whether the live playhead should be shown on the edited page.
static int playhead_ok(kria_engine_t* e, kria_grid_state_t* g) {
    return !g->meta_lock || g->edit_pattern == e->cfg.pattern;
}
static int step_in_loop(const kria_track_t* t, uint8_t p, uint8_t step) {
    uint8_t ls = t->lstart[p], le = t->lend[p];
    if (t->lswap[p]) return (step >= ls || step <= le);
    return (step >= ls && step <= le);
}
// Apply the loop shade Ansible adds to a lit body cell: -2 outside the loop,
// +1 inside when in modLoop (0 inside otherwise). Underflow-safe.
static void loop_shade(uint8_t* led, uint8_t idx, int in, int modloop) {
    if (!in) { led[idx] = (led[idx] >= 2) ? led[idx] - 2 : 0; }
    else if (modloop) { led[idx] += 1; }
}

void kria_grid_state_init(kria_grid_state_t* g) {
    memset(g, 0, sizeof(*g));
    g->mode = KR_P_TR;
    g->mod_mode = KR_MOD_NONE;
    g->track = 0;
    g->edit_pattern = 0;
    g->loop_last = -1;
    // Ansible default_kria edit-behavior flags.
    g->note_sync = 1;
    g->loop_sync = 2;
    g->div_sync = 0;
    g->note_div_sync = 0;
    g->scale_bank = NULL;
    g->mpseq = NULL;  // shell sets this to its working MP engine
    mp_grid_state_init(&g->mpgrid);
}

// ---- loop-range math (Ansible adjust_/update_loop_*) ----

static void adj_loop_start(kria_engine_t* e, kria_grid_state_t* g, uint8_t t,
                           uint8_t x, uint8_t m) {
    uint8_t ep = edit_pat(e, g);
    if (playhead_ok(e, g)) {
        int temp = (int)e->rt.pos[t][m] -
                   (int)e->cfg.p[e->cfg.pattern].t[t].lstart[m] + (int)x;
        if (temp < 0)
            temp += 16;
        else if (temp > 15)
            temp -= 16;
        e->rt.pos[t][m] = (uint8_t)temp;
    }
    kria_track_t* tk = &e->cfg.p[ep].t[t];
    tk->lstart[m] = x;
    int temp = (int)x + (int)tk->llen[m] - 1;
    if (temp > 15) {
        tk->lend[m] = (uint8_t)(temp - 16);
        tk->lswap[m] = 1;
    }
    else {
        tk->lend[m] = (uint8_t)temp;
        tk->lswap[m] = 0;
    }
}

static void adj_loop_end(kria_engine_t* e, kria_grid_state_t* g, uint8_t t,
                         uint8_t x, uint8_t m) {
    uint8_t ep = edit_pat(e, g);
    kria_track_t* tk = &e->cfg.p[ep].t[t];
    tk->lend[m] = x;
    int temp = (int)tk->lend[m] - (int)tk->lstart[m];
    if (temp < 0) {
        tk->llen[m] = (uint8_t)(temp + 17);
        tk->lswap[m] = 1;
    }
    else {
        tk->llen[m] = (uint8_t)(temp + 1);
        tk->lswap[m] = 0;
    }
    if (playhead_ok(e, g)) {
        kria_track_t* pk = &e->cfg.p[e->cfg.pattern].t[t];
        int p = e->rt.pos[t][m];
        if (pk->lswap[m]) {
            if (p < pk->lstart[m] && p > pk->lend[m])
                e->rt.pos[t][m] = pk->lstart[m];
        }
        else {
            if (p < pk->lstart[m] || p > pk->lend[m])
                e->rt.pos[t][m] = pk->lstart[m];
        }
    }
}

static void upd_loop_start(kria_engine_t* e, kria_grid_state_t* g, uint8_t t,
                           uint8_t x, uint8_t m) {
    uint8_t i, j;
    switch (g->loop_sync) {
        case 1:
            for (i = 0; i < KR_NUM_PARAMS; i++) adj_loop_start(e, g, t, x, i);
            break;
        case 2:
            for (j = 0; j < KR_NUM_TRACKS; j++)
                for (i = 0; i < KR_NUM_PARAMS; i++)
                    adj_loop_start(e, g, j, x, i);
            break;
        default: adj_loop_start(e, g, t, x, m); break;
    }
}

static void upd_loop_end(kria_engine_t* e, kria_grid_state_t* g, uint8_t t,
                         uint8_t x, uint8_t m) {
    uint8_t i, j;
    switch (g->loop_sync) {
        case 1:
            for (i = 0; i < KR_NUM_PARAMS; i++) adj_loop_end(e, g, t, x, i);
            break;
        case 2:
            for (j = 0; j < KR_NUM_TRACKS; j++)
                for (i = 0; i < KR_NUM_PARAMS; i++)
                    adj_loop_end(e, g, j, x, i);
            break;
        default: adj_loop_end(e, g, t, x, m); break;
    }
}

// Generic two-press loop gesture (Ansible pattern). `couple` is a second param
// to mirror the edit onto (note<->tr with note_sync), or -1 for none.
static void do_loop(kria_engine_t* e, kria_grid_state_t* g, uint8_t trk,
                    uint8_t param, int couple, uint8_t x, uint8_t z) {
    uint8_t ep = edit_pat(e, g);
    if (z) {
        if (g->loop_count == 0) {
            g->loop_first = x;
            g->loop_last = -1;
        }
        else {
            g->loop_last = x;
            upd_loop_start(e, g, trk, g->loop_first, param);
            upd_loop_end(e, g, trk, g->loop_last, param);
            if (couple >= 0) {
                upd_loop_start(e, g, trk, g->loop_first, (uint8_t)couple);
                upd_loop_end(e, g, trk, g->loop_last, (uint8_t)couple);
            }
        }
        g->loop_count++;
    }
    else {
        if (g->loop_count > 0) g->loop_count--;
        if (g->loop_count == 0 && g->loop_last == -1) {
            if (g->loop_first == e->cfg.p[ep].t[trk].lstart[param]) {
                upd_loop_start(e, g, trk, g->loop_first, param);
                upd_loop_end(e, g, trk, g->loop_first, param);
                if (couple >= 0) {
                    upd_loop_start(e, g, trk, g->loop_first, (uint8_t)couple);
                    upd_loop_end(e, g, trk, g->loop_first, (uint8_t)couple);
                }
            }
            else {
                upd_loop_start(e, g, trk, g->loop_first, param);
                if (couple >= 0)
                    upd_loop_start(e, g, trk, g->loop_first, (uint8_t)couple);
            }
        }
    }
}

// ---- tmul fan-out (Ansible kria_set_tmul) ----

static void set_track_tmul(kria_track_t* t, uint8_t mode, uint8_t nt,
                           uint8_t note_div_sync) {
    if (note_div_sync) {
        if (mode == KR_P_TR || mode == KR_P_NOTE) {
            t->tmul[KR_P_TR] = nt;
            t->tmul[KR_P_NOTE] = nt;
        }
        else {
            t->tmul[KR_P_RPT] = nt;
            t->tmul[KR_P_ALTNOTE] = nt;
            t->tmul[KR_P_OCT] = nt;
            t->tmul[KR_P_GLIDE] = nt;
            t->tmul[KR_P_DUR] = nt;
        }
    }
    else {
        uint8_t i;
        for (i = 0; i < KR_NUM_PARAMS; i++) t->tmul[i] = nt;
    }
}

static void set_tmul(kria_engine_t* e, kria_grid_state_t* g, uint8_t track,
                     uint8_t mode, uint8_t nt) {
    uint8_t ep = edit_pat(e, g);
    uint8_t i;
    switch (g->div_sync) {
        case 1:
            set_track_tmul(&e->cfg.p[ep].t[track], mode, nt, g->note_div_sync);
            break;
        case 2:
            for (i = 0; i < KR_NUM_TRACKS; i++)
                set_track_tmul(&e->cfg.p[ep].t[i], mode, nt, g->note_div_sync);
            break;
        default:
            e->cfg.p[ep].t[track].tmul[mode] = nt;
            if (g->note_div_sync) {
                if (mode == KR_P_TR) e->cfg.p[ep].t[track].tmul[KR_P_NOTE] = nt;
                if (mode == KR_P_NOTE) e->cfg.p[ep].t[track].tmul[KR_P_TR] = nt;
            }
            break;
    }
}

// ---- per-page rendering ----

static void draw_tr(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    int ph = playhead_ok(e, g);
    uint8_t i, j;
    for (i = 0; i < KR_NUM_TRACKS; i++) {
        kria_track_t* t = &e->cfg.p[ep].t[i];
        for (j = 0; j < 16; j++)
            if (t->tr[j]) led[i * 16 + j] = 3;
        if (ph) led[i * 16 + e->rt.pos[i][KR_P_TR]] += 4;
    }
    for (i = 0; i < KR_NUM_TRACKS; i++) {
        kria_track_t* t = &e->cfg.p[ep].t[i];
        uint8_t add = 2 + (g->mod_mode == KR_MOD_LOOP);
        if (t->lswap[KR_P_TR]) {
            for (j = 0; j < t->llen[KR_P_TR]; j++)
                led[i * 16 + ((j + t->lstart[KR_P_TR]) % 16)] += add;
        }
        else {
            for (j = t->lstart[KR_P_TR]; j <= t->lend[KR_P_TR] && j < 16; j++)
                led[i * 16 + j] += add;
        }
    }
}

static void draw_note(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    uint8_t nm = g->mode;  // KR_P_NOTE or KR_P_ALTNOTE
    kria_track_t* t = &e->cfg.p[ep].t[track];
    uint8_t* arr = (nm == KR_P_ALTNOTE) ? t->alt_note : t->note;
    uint8_t i;
    for (i = 0; i < 16; i++) {
        uint8_t v = arr[i] % 7;
        uint8_t cell = i + (6 - v) * 16;
        if (nm != KR_P_ALTNOTE && g->note_sync)
            led[cell] = t->tr[i] * 3;
        else
            led[cell] = 3;
    }
    if (playhead_ok(e, g)) {
        uint8_t p = e->rt.pos[track][nm];
        led[p + (6 - (arr[p] % 7)) * 16] += 4;
    }
    // loop highlight on each in-range step's note cell
    {
        uint8_t add = (g->mod_mode == KR_MOD_LOOP) ? 5 : 3;
        for (i = 0; i < 16; i++)
            if (step_in_loop(t, nm, i)) led[i + (6 - (arr[i] % 7)) * 16] += add;
    }
}

static void draw_oct(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    kria_track_t* t = &e->cfg.p[ep].t[track];
    int modloop = (g->mod_mode == KR_MOD_LOOP);
    uint8_t i;
    int j;
    memset(led, 2, 6);
    led[t->octshift] = L1;
    for (i = 0; i < 16; i++) {
        int octsum = sum_clip(t->oct[i], t->octshift, 5);
        int in = step_in_loop(t, KR_P_OCT, i);
        // Bar spans between the octshift baseline and octsum in EITHER
        // direction, so steps set below the baseline draw a downward bar
        // (Ansible refresh_kria octave view).
        int lo = t->octshift <= octsum ? t->octshift : octsum;
        int hi = t->octshift <= octsum ? octsum : t->octshift;
        for (j = lo; j <= hi; j++) {
            uint8_t idx = (uint8_t)(R6 - 16 * j + i);
            led[idx] = L0;
            loop_shade(led, idx, in, modloop);
        }
        if (playhead_ok(e, g) && i == e->rt.pos[track][KR_P_OCT])
            led[(uint8_t)(R6 - 16 * octsum + i)] += 4;
    }
}

static void draw_dur(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    kria_track_t* t = &e->cfg.p[ep].t[track];
    int modloop = (g->mod_mode == KR_MOD_LOOP);
    uint8_t i;
    int j;
    if (t->dur_mul >= 1) led[t->dur_mul - 1] = L1;
    for (i = 0; i < 16; i++) {
        int in = step_in_loop(t, KR_P_DUR, i);
        for (j = 0; j <= t->dur[i] && j <= 5; j++) {
            uint8_t idx = (uint8_t)(R1 + 16 * j + i);
            led[idx] = L0;
            loop_shade(led, idx, in, modloop);
        }
        if (playhead_ok(e, g) && i == e->rt.pos[track][KR_P_DUR]) {
            int j2 = t->dur[i] <= 5 ? t->dur[i] : 5;
            led[(uint8_t)(R1 + 16 * j2 + i)] += 4;
        }
    }
}

static void draw_rpt(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    kria_track_t* t = &e->cfg.p[ep].t[track];
    int modloop = (g->mod_mode == KR_MOD_LOOP);
    uint8_t i;
    int j;
    for (i = 0; i < 16; i++) {
        uint8_t rb = t->rptBits[i];
        int in = step_in_loop(t, KR_P_RPT, i);
        for (j = 0; j < 5; j++) {
            uint8_t idx = (uint8_t)(16 * (5 - j) + i);
            led[idx] = 0;
            if (rb & (1 << j)) led[idx] = L0;
            if (j < t->rpt[i]) led[idx] += 4;
            loop_shade(led, idx, in, modloop);
        }
        if (playhead_ok(e, g) && i == e->rt.pos[track][KR_P_RPT]) {
            int y = imax(
                1, (int)e->rt.activeRpt[track] - (int)e->rt.repeats[track]);
            if (y <= 5)
                led[(uint8_t)(R6 - 16 * y + i)] += (rb & (1 << y)) ? 4 : 2;
        }
        led[i] = 2;
        led[R6 + i] = 2;
    }
}

static void draw_glide(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    kria_track_t* t = &e->cfg.p[ep].t[track];
    int modloop = (g->mod_mode == KR_MOD_LOOP);
    uint8_t i;
    int j;
    for (i = 0; i < 16; i++) {
        int in = step_in_loop(t, KR_P_GLIDE, i);
        int gl = t->glide[i];
        for (j = 0; j <= gl && j <= 6; j++) {
            uint8_t idx = (uint8_t)(R6 - 16 * j + i);
            int v = L1 - (gl - j);
            led[idx] = (uint8_t)(v < 0 ? 0 : v);
            loop_shade(led, idx, in, modloop);
        }
        if (playhead_ok(e, g) && i == e->rt.pos[track][KR_P_GLIDE]) {
            int j2 = gl <= 6 ? gl : 6;
            led[(uint8_t)(R6 - 16 * j2 + i)] += 4;
        }
    }
}

static void draw_scale(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t y, x, i;
    for (y = 0; y < 4; y++) {
        kria_track_t* ty = &e->cfg.p[ep].t[y];
        led[0 + 16 * y] = ty->tt_clocked ? L1 : L0;
        led[1 + 16 * y] = ty->trigger_clocked ? L1 : L0;
        for (x = 3; x <= 7; x++)
            led[x + 16 * y] = (ty->direction == (x - 3)) ? 4 : 2;
    }
    for (i = 0; i < 7; i++) led[8 + 16 * i] = L0;
    for (i = 0; i < 8; i++) {
        led[R5 + i] = 2;
        led[R6 + i] = 2;
    }
    {
        uint8_t scale = e->cfg.p[ep].scale;
        led[R5 + (scale >> 3) * 16 + (scale & 0x7)] = L1;
        if (g->scale_bank) {
            for (i = 0; i < 7; i++) {
                uint8_t sp =
                    (uint8_t)(g->scale_bank[scale][i] + 8 + (6 - i) * 16);
                led[(uint8_t)(sp + e->rt.scale_adj[i])] = L0;
                led[sp] = L1;
            }
        }
    }
    if (g->scale_bank) {
        uint8_t ps = e->cfg.p[e->cfg.pattern].scale;
        for (i = 0; i < KR_NUM_TRACKS; i++) {
            if (e->cfg.p[e->cfg.pattern].t[i].tr[e->rt.pos[i][KR_P_TR]]) {
                uint8_t nd = e->rt.note[i] % 7;
                uint8_t sp = (uint8_t)(g->scale_bank[ps][nd] + 8 +
                                       e->rt.scale_adj[nd] + (6 - nd) * 16);
                led[sp]++;
            }
        }
    }
}

static void draw_pattern(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led) {
    if (!e->cfg.meta) {
        memset(led, 3, 16);
        led[e->cfg.pattern] = L1;
    }
    else {
        uint8_t steps = e->cfg.meta_steps[e->rt.meta_pos];
        if (steps > 15) steps = 15;
        memset(led + R6, 3, steps + 1);
        if (e->rt.meta_count <= 15) led[R6 + e->rt.meta_count] = L1;
        {
            uint8_t es = e->cfg.meta_steps[g->meta_edit];
            if (es <= 15) led[R6 + es] = L2;
        }
        led[e->cfg.pattern] = L0;
        led[e->cfg.meta_pat[g->meta_edit]] = L1;
        if (!e->cfg.meta_lswap) {
            memset(led + R2, 3, e->cfg.meta_len <= 64 ? e->cfg.meta_len : 64);
        }
        else {
            memset(led + R2, 3, e->cfg.meta_end <= 63 ? e->cfg.meta_end : 63);
            memset(led + R2 + e->cfg.meta_start, 3, 64 - e->cfg.meta_start);
        }
        led[R2 + e->rt.meta_pos] = L1;
        led[R2 + g->meta_edit] = L2;
        if (e->rt.meta_next) {
            led[R2 + e->rt.meta_next - 1] = L2;
            led[e->cfg.meta_pat[e->rt.meta_next]] = L2;
        }
    }
    if (e->rt.cue_pat_next) led[e->rt.cue_pat_next - 1] = L2;
    if (g->meta_lock && g->meta_lock_blink) led[edit_pat(e, g)] += 4;
    if (g->mod_mode == KR_MOD_TIME) {
        if (e->rt.cue_count <= 15) led[R1 + e->rt.cue_count] = L0;
        if (e->cfg.cue_div <= 15) led[R1 + e->cfg.cue_div] = L1;
    }
    else {
        if (e->cfg.cue_steps <= 15) led[R1 + e->cfg.cue_steps] = L0;
        if (e->rt.cue_count <= 15) led[R1 + e->rt.cue_count] = L1;
    }
}

// Meadowphysics-style cascade sequencer (KR_MODE_MPSEQ): 6 lanes (rows 0-5)
// fire scripts 3-8. Reuses the standalone MP grid render verbatim: render into
// a scratch buffer (mp_grid_refresh owns the whole 128-cell buffer + finalizes
// it), then copy only rows 0-5 over the Kria page -- leaving row 6 blank and the
// row-7 nav bar (already drawn by draw_bottom_row) intact. The outer
// grid_led_finalize re-runs on the copied cells, which is idempotent.
static void draw_mpseq(kria_grid_state_t* g, uint8_t* led) {
    if (!g->mpseq) return;
    uint8_t scratch[GRID_LED_COUNT];
    mp_grid_refresh(g->mpseq, &g->mpgrid, scratch, 1);  // vari=1: keep the ramp
    memcpy(led, scratch, R6);  // rows 0-5 (indices 0..R6-1); row 6/7 untouched
}

// Mod overlay drawn before the page: returns 1 if it fully replaces the page.
static int draw_mod_overlay(kria_engine_t* e, kria_grid_state_t* g,
                            uint8_t* led) {
    uint8_t ep = edit_pat(e, g);
    uint8_t mode = g->mode, track = g->track, i;
    // The MP-seq page ignores the LOOP/TIME/PROB mods (MP has its own col0/col1
    // view gestures); never overlay it.
    if (mode == KR_MODE_MPSEQ) return 0;
    switch (g->mod_mode) {
        case KR_MOD_LOOP: led[R7 + 10] = L1; return 0;
        case KR_MOD_TIME:
            led[R7 + 11] = L1;
            memset(led + R1, 3, 16);
            if (mode < KR_NUM_PARAMS) {
                uint8_t tm = e->cfg.p[ep].t[track].tmul[mode];
                if (tm >= 1 && tm <= 16) led[R1 + tm - 1] = L1;
            }
            else if (mode == KR_MODE_PATTERN) {
                if (e->cfg.cue_div <= 15) led[R1 + e->cfg.cue_div] = L1;
            }
            return 1;
        case KR_MOD_PROB:
            led[R7 + 12] = L1;
            memset(led + R5, 3, 16);
            if (mode < KR_NUM_PARAMS) {
                for (i = 0; i < 16; i++) {
                    uint8_t w = e->cfg.p[ep].t[track].p[mode][i];
                    if (w) {
                        int hit = (ep == e->cfg.pattern &&
                                   i == e->rt.pos[track][mode]);
                        led[(5 - w) * 16 + i] = hit ? 10 : 6;
                    }
                }
            }
            return 1;
        default: return 0;
    }
}

static void draw_bottom_row(kria_engine_t* e, kria_grid_state_t* g,
                            uint8_t* led) {
    uint8_t mode = g->mode, i;
    int idx;
    memset(led + R7 + 5, L0, 4);  // x=5..8
    led[R7 + 10] = L0;
    led[R7 + 11] = L0;
    if (mode < KR_NUM_PARAMS) led[R7 + 12] = L0;  // PROB selector (not on MPseq)
    led[R7 + 14] = L0;
    led[R7 + 15] =
        (e->cfg.meta && g->meta_lock && g->meta_lock_blink) ? L1 : L0;

    for (i = 0; i < KR_NUM_TRACKS; i++) {
        if (e->rt.mutes[i])
            led[R7 + i] = (g->track == i) ? L1 : 2;
        else
            led[R7 + i] = (g->track == i) ? L2 : L0;
        if (!e->rt.mutes[i]) led[R7 + i] += g->blinks[i] * 2;
    }

    switch (mode) {
        case KR_P_TR:
        case KR_P_RPT: idx = R7 + 5; break;
        case KR_P_NOTE:
        case KR_P_ALTNOTE: idx = R7 + 6; break;
        case KR_P_OCT:
        case KR_P_GLIDE: idx = R7 + 7; break;
        case KR_P_DUR:
        case KR_MODE_MPSEQ: idx = R7 + 8; break;  // shared DUR selector
        case KR_MODE_SCALE: idx = R7 + 14; break;
        case KR_MODE_PATTERN: idx = R7 + 15; break;
        default: idx = R7 + 0; break;
    }
    if (mode == KR_MODE_PATTERN)
        led[idx] =
            (e->cfg.meta && g->meta_lock && g->meta_lock_blink) ? L1 : L2;
    else {
        int is_alt =
            (mode == KR_P_RPT || mode == KR_P_ALTNOTE || mode == KR_P_GLIDE ||
             mode == KR_MODE_MPSEQ);
        led[idx] = (is_alt && g->alt_blink) ? L1 : L2;
    }
}

void kria_grid_refresh(kria_engine_t* e, kria_grid_state_t* g, uint8_t* led,
                       uint8_t vari) {
    memset(led, 0, 128);
    draw_bottom_row(e, g, led);

    if (!draw_mod_overlay(e, g, led)) {
        switch (g->mode) {
            case KR_P_TR: draw_tr(e, g, led); break;
            case KR_P_NOTE:
            case KR_P_ALTNOTE: draw_note(e, g, led); break;
            case KR_P_OCT: draw_oct(e, g, led); break;
            case KR_P_DUR: draw_dur(e, g, led); break;
            case KR_MODE_MPSEQ: draw_mpseq(g, led); break;
            case KR_P_RPT: draw_rpt(e, g, led); break;
            case KR_P_GLIDE: draw_glide(e, g, led); break;
            case KR_MODE_SCALE: draw_scale(e, g, led); break;
            case KR_MODE_PATTERN: draw_pattern(e, g, led); break;
            default: break;
        }
    }

    grid_led_finalize(led, vari);
}

// ---- key handling ----

static void key_bottom_row(kria_engine_t* e, kria_grid_state_t* g, uint8_t x,
                           uint8_t z) {
    uint8_t mode = g->mode;
    if (z) {
        g->hold_pending = 0;  // abandon any pending pattern hold on page change
        if (x < 4) {
            if (g->mod_mode == KR_MOD_LOOP)
                e->rt.mutes[x] = !e->rt.mutes[x];
            else
                g->track = x;
        }
        else if (x == 5)
            g->mode = (mode == KR_P_TR) ? KR_P_RPT : KR_P_TR;
        else if (x == 6)
            g->mode = (mode == KR_P_NOTE) ? KR_P_ALTNOTE : KR_P_NOTE;
        else if (x == 7)
            g->mode = (mode == KR_P_OCT) ? KR_P_GLIDE : KR_P_OCT;
        else if (x == 8)
            g->mode = (mode == KR_P_DUR) ? KR_MODE_MPSEQ : KR_P_DUR;
        // The MP-seq page ignores LOOP/TIME/PROB (MP has its own view gestures).
        else if (x == 10 && mode != KR_MODE_MPSEQ) {
            g->mod_mode = KR_MOD_LOOP;
            g->loop_count = 0;
        }
        else if (x == 11 && mode != KR_MODE_MPSEQ)
            g->mod_mode = KR_MOD_TIME;
        else if (x == 12) {
            if (mode < KR_NUM_PARAMS) g->mod_mode = KR_MOD_PROB;
        }
        else if (x == 14)
            g->mode = KR_MODE_SCALE;
        else if (x == 15) {
            g->mode = KR_MODE_PATTERN;
            g->cue = 1;
        }
    }
    else {
        if (x == 10 || x == 11 || x == 12)
            g->mod_mode = KR_MOD_NONE;
        else if (x == 15)
            g->cue = 0;
    }

    // Leaving the MP-seq page mid-hold (a nav press while holding the col0/col1
    // positions/speed/rules gesture) orphans the MP release events, stranding
    // mpgrid in a held sub-view -- the page then "hangs" on return. Reset the
    // MP grid hold state on any exit from the MP-seq page.
    if (mode == KR_MODE_MPSEQ && g->mode != KR_MODE_MPSEQ)
        mp_grid_state_init(&g->mpgrid);
}

void kria_grid_process_key(kria_engine_t* e, kria_grid_state_t* g, uint8_t x,
                           uint8_t y, uint8_t z) {
    if (x >= 16 || y >= 8) return;
    if (y == 7) {
        key_bottom_row(e, g, x, z);
        return;
    }

    uint8_t ep = edit_pat(e, g);
    uint8_t track = g->track;
    uint8_t mode = g->mode;
    uint8_t mm = g->mod_mode;
    kria_track_t* t = &e->cfg.p[ep].t[track];

    switch (mode) {
        case KR_P_TR:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z) e->cfg.p[ep].t[y].tr[x] ^= 1;  // row y = track
                    break;
                case KR_MOD_LOOP:
                    if (z && y < 4) {
                        if (g->loop_count == 0) {
                            g->loop_edit = y;
                            g->loop_first = x;
                            g->loop_last = -1;
                        }
                        else {
                            g->loop_last = x;
                            upd_loop_start(e, g, g->loop_edit, g->loop_first,
                                           KR_P_TR);
                            upd_loop_end(e, g, g->loop_edit, g->loop_last,
                                         KR_P_TR);
                            if (g->note_sync) {
                                upd_loop_start(e, g, g->loop_edit,
                                               g->loop_first, KR_P_NOTE);
                                upd_loop_end(e, g, g->loop_edit, g->loop_last,
                                             KR_P_NOTE);
                            }
                        }
                        g->loop_count++;
                    }
                    else if (!z && g->loop_edit == y) {
                        if (g->loop_count > 0) g->loop_count--;
                        if (g->loop_count == 0 && g->loop_last == -1) {
                            uint8_t le = g->loop_edit, lf = g->loop_first;
                            if (lf == e->cfg.p[ep].t[le].lstart[KR_P_TR]) {
                                upd_loop_start(e, g, le, lf, KR_P_TR);
                                upd_loop_end(e, g, le, lf, KR_P_TR);
                                if (g->note_sync) {
                                    upd_loop_start(e, g, le, lf, KR_P_NOTE);
                                    upd_loop_end(e, g, le, lf, KR_P_NOTE);
                                }
                            }
                            else {
                                upd_loop_start(e, g, le, lf, KR_P_TR);
                                if (g->note_sync)
                                    upd_loop_start(e, g, le, lf, KR_P_NOTE);
                            }
                        }
                    }
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, KR_P_TR, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6)
                        e->cfg.p[ep].t[track].p[KR_P_TR][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_P_NOTE:
        case KR_P_ALTNOTE:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z && y <= 6) {
                        if (mode == KR_P_NOTE && g->note_sync) {
                            if (t->tr[x] && t->note[x] == 6 - y)
                                t->tr[x] = 0;
                            else {
                                t->tr[x] = 1;
                                t->note[x] = 6 - y;
                            }
                        }
                        else if (mode == KR_P_ALTNOTE)
                            t->alt_note[x] = 6 - y;
                        else
                            t->note[x] = 6 - y;
                    }
                    break;
                case KR_MOD_LOOP:
                    do_loop(e, g, track, mode,
                            (mode == KR_P_NOTE && g->note_sync) ? KR_P_TR : -1,
                            x, z);
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, mode, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6) t->p[mode][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_P_OCT:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z) {
                        if (y == 0) {
                            if (x <= 5) t->octshift = x;
                        }
                        else if (y <= 6)
                            t->oct[x] = (int8_t)((6 - y) - t->octshift);
                    }
                    break;
                case KR_MOD_LOOP:
                    do_loop(e, g, track, KR_P_OCT, -1, x, z);
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, KR_P_OCT, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6) t->p[KR_P_OCT][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_P_DUR:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z) {
                        if (y == 0)
                            t->dur_mul = x + 1;
                        else
                            t->dur[x] = y - 1;
                    }
                    break;
                case KR_MOD_LOOP:
                    if (y > 0) do_loop(e, g, track, KR_P_DUR, -1, x, z);
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, KR_P_DUR, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6) t->p[KR_P_DUR][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_P_RPT:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z) {
                        if (y == 0) {
                            if (t->rpt[x] < 5) t->rpt[x]++;
                        }
                        else if (y >= 1 && y <= 5)
                            t->rptBits[x] ^= (1 << (5 - y));
                        else if (y == 6) {
                            if (t->rpt[x] > 1) t->rpt[x]--;
                        }
                    }
                    break;
                case KR_MOD_LOOP:
                    do_loop(e, g, track, KR_P_RPT, -1, x,
                            z);  // TODO vrange gesture
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, KR_P_RPT, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6) t->p[KR_P_RPT][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_P_GLIDE:
            switch (mm) {
                case KR_MOD_NONE:
                    if (z && y <= 6) t->glide[x] = 6 - y;
                    break;
                case KR_MOD_LOOP:
                    do_loop(e, g, track, KR_P_GLIDE, -1, x, z);
                    break;
                case KR_MOD_TIME:
                    if (z) set_tmul(e, g, track, KR_P_GLIDE, x + 1);
                    break;
                case KR_MOD_PROB:
                    if (z && y > 1 && y < 6) t->p[KR_P_GLIDE][x] = 5 - y;
                    break;
                default: break;
            }
            break;

        case KR_MODE_MPSEQ:
            // Meadowphysics cascade seq: rows 0-5 are the six lanes (scripts
            // 3-8), row 6 is unused, row 7 (nav) was handled above. Forward
            // straight to the reused MP grid key handler; MP's own col0/col1
            // hold-gestures switch its positions/speed/rules views. Mods are
            // ignored here (blocked in key_bottom_row).
            if (g->mpseq && y < KR_SCRIPT_LANES) {
                // In the rules view MP picks the rule by ROW (rules[er] = y),
                // which needs rows 6/7 -- rows Kria doesn't own -- so POLE(6)
                // and STOP(7) are unreachable. Select the rule by COLUMN
                // instead: cols 8-15 map to rules 0-7, all reachable in the
                // 6-lane window. Col 7 (MP's stray rule-select edge) is
                // dropped so it can't set a row-based rule. Dest/target
                // (cols 4-6) and the col0/col1 holds still forward to MP.
                if (g->mpgrid.edit_mode == MP_GRID_RULES && x >= 7) {
                    if (z && x >= 8)
                        g->mpseq->cfg.rules[g->mpgrid.edit_row] = x - 8;
                }
                else
                    mp_grid_process_key(g->mpseq, &g->mpgrid, x, y, z);
            }
            break;

        case KR_MODE_SCALE:
            if (z) {
                if (y < 4 && x <= 7) {
                    kria_track_t* ty = &e->cfg.p[ep].t[y];
                    if (x == 0)
                        ty->tt_clocked = !ty->tt_clocked;
                    else if (x == 1)
                        ty->trigger_clocked = !ty->trigger_clocked;
                    else if (x >= 3 && x <= 7)
                        ty->direction = x - 3;
                }
                else if (x < 8 && y > 4) {
                    uint8_t s = (y - 5) * 8 + x;
                    if (s < 16) {
                        e->cfg.p[ep].scale = s;
                        memset(e->rt.scale_adj, 0, 8);
                        if (g->scale_bank)
                            kria_engine_calc_scale(e, g->scale_bank[s]);
                    }
                }
                else if (x >= 8 && y <= 6) {
                    if (g->scale_bank) {
                        uint8_t deg = 6 - y;
                        uint8_t v = x - 8;
                        if (v > 7) v = 7;
                        g->scale_bank[e->cfg.p[ep].scale][deg] = v;
                        kria_engine_calc_scale(
                            e, g->scale_bank[e->cfg.p[ep].scale]);
                    }
                }
            }
            break;

        case KR_MODE_PATTERN:
            if (y == 0) {
                // Plain pattern select (no cue, no meta) is deferred so the
                // long-press copy gesture can share the button: a quick release
                // switches; holding fires kria_grid_pattern_hold_fire (shell-
                // timed). The cue/meta row-0 actions stay press-driven.
                if (!g->cue && !e->cfg.meta) {
                    if (z) {
                        g->hold_pending = 1;
                        g->hold_x = x;
                    }
                    else if (g->hold_pending && g->hold_x == x) {
                        kria_engine_change_pattern(e, x);
                        if (!g->meta_lock) g->edit_pattern = x;
                        g->hold_pending = 0;
                    }
                }
                else if (z) {
                    if (g->cue && e->cfg.meta) {
                        g->meta_lock = !g->meta_lock;
                        if (g->meta_lock) g->edit_pattern = e->cfg.pattern;
                    }
                    else if (g->cue)  // cue && !meta
                        e->rt.cue_pat_next = x + 1;
                    else  // meta && !cue
                        e->cfg.meta_pat[g->meta_edit] = x;
                }
            }
            else if (z) {
                if (y == 1) {
                    if (mm == KR_MOD_TIME)
                        e->cfg.cue_div = x;
                    else
                        e->cfg.cue_steps = x;
                }
                else if (y >= 2 && y <= 5) {
                    uint8_t slot = (y - 2) * 16 + x;
                    if (slot < 64) {
                        if (g->cue)
                            e->rt.meta_next = slot + 1;
                        else if (mm != KR_MOD_LOOP)  // TODO meta loop gesture
                            g->meta_edit = slot;
                    }
                }
                else if (y == 6) {
                    if (g->cue) {
                        e->cfg.meta = !e->cfg.meta;
                        g->meta_lock = 0;
                    }
                    else
                        e->cfg.meta_steps[g->meta_edit] = x;
                }
            }
            break;

        default: break;
    }
}

uint8_t kria_grid_pattern_hold_fire(kria_engine_t* e, kria_grid_state_t* g) {
    if (!g->hold_pending) return 0;
    uint8_t slot = g->hold_x;
    g->hold_pending = 0;
    if (slot >= KR_NUM_PATTERNS) return 0;
    // Copy the playing pattern into the held slot, then switch to it (Ansible
    // grid_keytimer_kria mPattern). Skip the self-copy when the slot is already
    // current -- a plain overlapping memcpy would be undefined.
    if (slot != e->cfg.pattern)
        memcpy(&e->cfg.p[slot], &e->cfg.p[e->cfg.pattern],
               sizeof(e->cfg.p[slot]));
    kria_engine_change_pattern(e, slot);
    if (!g->meta_lock) g->edit_pattern = slot;
    return 1;
}
