// Earthsea engine, ported from Ansible v3.2 src/ansible_grid.c (ES app,
// lines ~4625-5795). See es_engine.h for the architecture notes and
// EARTHSEA_PORT_PLAN.md for the port plan.
//
// Deliberate deviations from Ansible (all noted inline):
//  - no timers / no get_ticks: wall time is injected (`now`), playback
//    stepping returns the next interval for the caller to schedule;
//  - clock_external is a runtime field set by the shell, not a global;
//  - default_es's i/j index slip is fixed (every pattern gets defaults);
//  - e.octave is stored/edited but never applied to pitch -- faithful to
//    upstream, where the voices-view octave buttons are inert.

#include "es_engine.h"

#include <string.h>

// ---- helpers -------------------------------------------------------------

static es_pattern_t* sel(es_engine_t* e) {
    return &e->cfg.p[e->cfg.p_select];
}

// Interval after event idx, with the linearize transform applied: sub-chord
// gaps collapse to 1 tick, everything else becomes the reference interval.
static uint16_t lin_interval(const es_pattern_t* p, uint16_t idx) {
    uint16_t interval = p->e[idx].interval;
    if (p->linearize) {
        if (interval < ES_CHORD_THRESHOLD)
            interval = 1;
        else
            interval = p->e[p->interval_ind].interval;
    }
    return interval;
}

static void update_total_time(es_engine_t* e) {
    const es_pattern_t* p = sel(e);
    e->rt.p_total = 0;
    for (uint16_t i = 0; i < p->length; i++)
        e->rt.p_total += lin_interval(p, i);
}

// ---- voices --------------------------------------------------------------

int16_t es_engine_note_index(int8_t x, int8_t y) {
    int16_t n = x + (7 - y) * 5 - 1;  // fourths layout, 8 rows
    if (n < 0) n = 0;
    if (n > 119) n = 119;
    return n;
}

// Ansible es_note_off_i drops the gate unconditionally (clr_tr on an idle
// voice is a hardware no-op there). Here the vtable only sees offs for
// active voices -- same jack-level behavior, incl. the retrigger when a
// sounding voice is stolen, without spurious note_off noise downstream
// (i2c/MIDI followers would otherwise emit stray note-offs).
static void note_off_i(es_engine_t* e, uint8_t v) {
    uint8_t was_active = e->rt.notes[v].active;
    e->rt.notes[v].active = 0;
    if (was_active && e->out.note_off) e->out.note_off(e->out.ctx, v);
}

static void note_off_xy(es_engine_t* e, int8_t x, int8_t y) {
    for (uint8_t v = 0; v < ES_NUM_VOICES; v++)
        if (e->rt.notes[v].x == x && e->rt.notes[v].y == y) note_off_i(e, v);
}

// Ansible es_note_on: reuse the voice already sounding this key, else first
// free voice in the mask, else steal the oldest.
static void note_on_xy(es_engine_t* e, int8_t x, int8_t y, uint8_t from_pattern,
                       uint16_t duration, uint8_t voices, uint32_t now) {
    uint8_t v = 255;
    for (uint8_t i = 0; i < ES_NUM_VOICES; i++)
        if ((voices & (1 << i)) &&
            (!e->rt.notes[i].active ||
             (e->rt.notes[i].x == x && e->rt.notes[i].y == y))) {
            v = i;
            break;
        }

    if (v == 255) {
        uint32_t earliest = 0xffffffff;
        for (uint8_t i = 0; i < ES_NUM_VOICES; i++)
            if ((voices & (1 << i)) && e->rt.notes[i].start < earliest) {
                earliest = e->rt.notes[i].start;
                v = i;
            }
    }

    if (v == 255) return;

    note_off_i(e, v);

    e->rt.notes[v].active = 1;
    e->rt.notes[v].x = x;
    e->rt.notes[v].y = y;
    e->rt.notes[v].start = now;
    e->rt.notes[v].from_pattern = from_pattern;

    if (e->out.note_on)
        e->out.note_on(e->out.ctx, v, es_engine_note_index(x, y), duration);
}

void es_engine_note_off_voice(es_engine_t* e, uint8_t voice) {
    if (voice >= ES_NUM_VOICES) return;
    note_off_i(e, voice);
}

void es_engine_kill_all_notes(es_engine_t* e) {
    for (uint8_t v = 0; v < ES_NUM_VOICES; v++) note_off_i(e, v);
}

void es_engine_kill_pattern_notes(es_engine_t* e) {
    for (uint8_t v = 0; v < ES_NUM_VOICES; v++)
        if (e->rt.notes[v].from_pattern) note_off_i(e, v);
}

// ---- recording -----------------------------------------------------------

static void complete_recording(es_engine_t* e, uint32_t now) {
    es_pattern_t* p = sel(e);
    if (!p->length) return;

    p->e[p->length - 1].interval = (uint16_t)(now - e->rt.rec_tick);

    for (uint16_t i = 0; i < p->length; i++) {
        if (p->e[i].interval > ES_CHORD_THRESHOLD) {
            p->interval_ind = i;
            break;
        }
    }
}

static void record_pattern_note(es_engine_t* e, uint8_t x, uint8_t y,
                                uint8_t on, uint32_t now) {
    es_pattern_t* p = sel(e);
    uint16_t l = p->length;
    if (l >= ES_EVENTS_PER_PATTERN) {
        complete_recording(e, now);  // updates the last event's interval
        return;
    }

    if (!l) {
        p->root_x = x;
        p->root_y = y;
    }

    if (l) p->e[l - 1].interval = (uint16_t)(now - e->rt.rec_tick);
    e->rt.rec_tick = now;

    p->e[l].index = x + (y << 4);
    if (x == 15 && y == 0)  // rest
        p->e[l].on = on ? 3 : 2;
    else
        p->e[l].on = on ? 1 : 0;
    p->length++;
}

static void start_recording(es_engine_t* e) {
    es_pattern_t* p = sel(e);
    p->length = 0;
    p->start = 0;
    p->end = 15;
    p->dir = 0;
    e->rt.mode = es_recording;
}

// ---- playback ------------------------------------------------------------

// Emit the event at rt.pos, transposed so the pattern's first event lands on
// root_x/root_y (Ansible es_play_pattern_note).
static void play_pattern_note(es_engine_t* e, uint32_t now) {
    es_pattern_t* p = sel(e);
    uint16_t i = p->dir ? p->length - 1 : 0;
    uint8_t first_x = p->e[i].index & 15;
    uint8_t first_y = p->e[i].index >> 4;
    int16_t x = (p->e[e->rt.pos].index & 15) + p->root_x - first_x;
    int16_t y = (p->e[e->rt.pos].index >> 4) + p->root_y - first_y;

    if (p->e[e->rt.pos].on == 1)
        note_on_xy(e, (int8_t)x, (int8_t)y, 1,
                   p->edge == ES_EDGE_FIXED ? p->edge_time : 0, p->voices,
                   now);
    else if (p->e[e->rt.pos].on == 0 && p->edge == ES_EDGE_PATTERN)
        note_off_xy(e, (int8_t)x, (int8_t)y);
}

// Advance rt.pos; returns 1 when a non-looping pattern just ended.
static uint8_t next_note(es_engine_t* e, uint32_t now) {
    es_pattern_t* p = sel(e);
    if (++e->rt.pos >= p->length) {
        e->rt.pos = 0;
        e->rt.p_start = now;
        if (!p->loop) {
            es_engine_kill_pattern_notes(e);
            e->rt.mode = es_stopped;
            return 1;
        }
    }
    return 0;
}

uint32_t es_engine_play_advance(es_engine_t* e, uint32_t now) {
    if (e->rt.mode != es_playing) return 0;
    if (e->rt.clock_external) return 0;
    if (next_note(e, now)) return 0;

    uint16_t interval = lin_interval(sel(e), e->rt.pos);
    if (!interval) interval = 1;
    play_pattern_note(e, now);
    return interval;
}

void es_engine_stop_playback(es_engine_t* e) {
    e->rt.mode = es_stopped;
    es_engine_kill_pattern_notes(e);
}

uint32_t es_engine_start_playback(es_engine_t* e, uint8_t pos, uint32_t now) {
    es_pattern_t* p = sel(e);

    if (e->rt.mode == es_playing) es_engine_stop_playback(e);
    else if (e->rt.mode == es_recording) complete_recording(e, now);

    if (!p->length) {
        e->rt.mode = es_stopped;
        return 0;
    }

    e->rt.mode = es_playing;

    uint32_t interval;

    if (pos) {
        // Scrub start: walk to the event covering p_total*pos/16 and return
        // the remaining part of its interval. No immediate note (the timer
        // fires the next event), matching Ansible.
        uint32_t start = (e->rt.p_total * pos) >> 4;
        uint32_t tick = 0;
        interval = 0;
        for (e->rt.pos = 0; e->rt.pos < p->length; e->rt.pos++) {
            interval = lin_interval(p, e->rt.pos);
            if (tick + interval > start) break;
            tick += interval;
        }
        if (e->rt.pos >= p->length) {
            e->rt.pos = p->length - 1;
            interval = 1;
        }
        else {
            interval = tick + interval - start;
            if (!interval) interval = 1;
        }

        if (e->rt.clock_external) return 0;

        e->rt.p_start = now - start;
        return interval;
    }

    e->rt.pos = 0;
    e->rt.p_start = now;
    update_total_time(e);

    if (e->rt.clock_external) return 0;

    interval = lin_interval(p, 0);
    if (!interval) interval = 1;
    play_pattern_note(e, now);
    return interval;
}

void es_engine_clock_step(es_engine_t* e, uint32_t now) {
    // Ansible handler_ESTr input-1 / ii ES_CLOCK: play the whole chord group
    // at rt.pos in one shot, then advance past it.
    es_pattern_t* p = sel(e);
    if (!p->length) return;

    uint16_t i = p->length;
    while (i > 0) {
        i--;
        play_pattern_note(e, now);
        if (p->e[e->rt.pos].interval > ES_CHORD_THRESHOLD) break;
        if (++e->rt.pos >= p->length) {
            e->rt.pos--;
            break;
        }
    }
    if (++e->rt.pos >= p->length) {
        e->rt.pos = 0;
        if (!p->loop) {
            es_engine_kill_pattern_notes(e);
            e->rt.mode = es_stopped;
        }
    }
}

// ---- transport -----------------------------------------------------------

void es_engine_arm(es_engine_t* e) {
    if (e->rt.mode == es_stopped) { e->rt.mode = es_armed; }
    else if (e->rt.mode == es_playing) {
        es_engine_stop_playback(e);
        e->rt.mode = es_armed;
    }
}

void es_engine_stop_recording(es_engine_t* e, uint32_t now) {
    if (e->rt.mode == es_recording) {
        complete_recording(e, now);
        e->rt.mode = es_stopped;
    }
    else if (e->rt.mode == es_armed) { e->rt.mode = es_stopped; }
}

// ---- live keyboard --------------------------------------------------------

uint32_t es_engine_grid_press(es_engine_t* e, uint8_t x, uint8_t y, uint8_t z,
                              bool rest_held, uint32_t now) {
    es_pattern_t* p = sel(e);

    // Ansible handler_ESGridKey tail: armed -> recording on any key event;
    // recording captures both edges (rests included) and still monitors live.
    if (e->rt.mode == es_armed) start_recording(e);
    if (e->rt.mode == es_recording) record_pattern_note(e, x, y, z, now);

    if (e->cfg.arp && e->rt.mode != es_recording) {
        if (!z) return 0;
        p->root_x = x;
        p->root_y = y;
        return es_engine_start_playback(e, 0, now);
    }
    else if (e->rt.mode == es_stopped && rest_held && z) {
        uint8_t i = (y << 4) + x;
        if (i < ES_KEYMAP_SIZE)
            e->cfg.keymap[i] = (e->cfg.keymap[i] + 1) % 3;
    }
    else {
        if (p->edge == ES_EDGE_DRONE) {
            if (z) {
                uint8_t found = 0;
                for (uint8_t v = 0; v < ES_NUM_VOICES; v++)
                    if (x == e->rt.notes[v].x && y == e->rt.notes[v].y &&
                        e->rt.notes[v].active) {
                        note_off_xy(e, x, y);
                        found = 1;
                    }
                if (!found) note_on_xy(e, x, y, 0, 0, e->cfg.voices, now);
            }
        }
        else {
            if (z) {
                if (x != 15 || y != 0)
                    note_on_xy(e, x, y, 0, 0,
                               e->rt.mode == es_recording ? p->voices
                                                          : e->cfg.voices,
                               now);
            }
            else
                note_off_xy(e, x, y);
        }
    }
    return 0;
}

// ---- pattern ops -----------------------------------------------------------

void es_engine_set_pattern(es_engine_t* e, uint8_t pattern) {
    if (pattern >= ES_NUM_PATTERNS) return;
    e->cfg.p_select = pattern;
}

void es_engine_double_speed(es_engine_t* e) {
    es_pattern_t* p = sel(e);
    for (uint16_t i = 0; i < p->length; i++) {
        if (p->e[i].interval > (ES_CHORD_THRESHOLD << 1))
            p->e[i].interval >>= 1;
        else if (p->e[i].interval > ES_CHORD_THRESHOLD)
            p->e[i].interval = ES_CHORD_THRESHOLD + 1;
    }
    update_total_time(e);
}

void es_engine_half_speed(es_engine_t* e) {
    es_pattern_t* p = sel(e);
    for (uint16_t i = 0; i < p->length; i++)
        if (p->e[i].interval > ES_CHORD_THRESHOLD) {
            uint16_t interval = p->e[i].interval << 1;
            // saturate instead of wrapping (Ansible overflow guard)
            if (interval > p->e[i].interval) p->e[i].interval = interval;
        }
    update_total_time(e);
}

void es_engine_set_linearize(es_engine_t* e, uint8_t on) {
    sel(e)->linearize = on ? 1 : 0;
    update_total_time(e);
}

// Ansible es_reverse: reverse the event list, flipping on<->off (a reversed
// timeline releases where it pressed), and re-derive intervals + the
// linearize reference index.
static void reverse_pattern(es_engine_t* e) {
    es_pattern_t* p = sel(e);
    uint16_t l = p->length;
    if (!l) return;

    es_event_t te[ES_EVENTS_PER_PATTERN];

    for (uint16_t i = 0; i < l; i++) {
        te[i] = p->e[i];
        if (te[i].on == 3) te[i].on = 2;
        else if (te[i].on == 2) te[i].on = 3;
        else if (te[i].on == 1) te[i].on = 0;
        else if (te[i].on == 0) te[i].on = 1;
    }

    for (uint16_t i = 0; i < l; i++) p->e[i] = te[l - i - 1];
    for (uint16_t i = 0; i + 1 < l; i++)
        p->e[i].interval = te[l - i - 2].interval;
    p->e[l - 1].interval = te[l - 1].interval;

    if (p->dir) {
        for (int16_t i = (int16_t)l - 1; i >= 0; i--)
            if ((p->e[i].on == 3 || p->e[i].on == 1) &&
                p->e[i].interval > ES_CHORD_THRESHOLD) {
                p->interval_ind = i;
                break;
            }
    }
    else {
        for (uint16_t i = 0; i < l; i++)
            if ((p->e[i].on == 3 || p->e[i].on == 1) &&
                p->e[i].interval > ES_CHORD_THRESHOLD) {
                p->interval_ind = i;
                break;
            }
    }
}

void es_engine_set_direction(es_engine_t* e, uint8_t rev) {
    es_pattern_t* p = sel(e);
    rev = rev ? 1 : 0;
    if (p->dir != rev) reverse_pattern(e);
    p->dir = rev;
    if (e->rt.mode == es_playing) es_engine_kill_pattern_notes(e);
}

void es_engine_transpose(es_engine_t* e, int16_t delta) {
    // ii ES_TRANS: walk the root along the fourths layout, staying on the
    // playable field (x 1..15, y 1..7 at the extremes).
    es_pattern_t* p = sel(e);
    if (delta > 0) {
        for (int16_t i = 0; i < delta; i++) {
            if (p->root_y == 1 && p->root_x == 15) break;
            p->root_x++;
            if (p->root_x == 16) {
                p->root_x = 11;
                p->root_y--;
            }
        }
    }
    else {
        for (int16_t i = 0; i < -delta; i++) {
            if (p->root_y == 7 && p->root_x == 1) break;
            p->root_x--;
            if (p->root_x == 0) {
                p->root_x = 5;
                p->root_y++;
            }
        }
    }
}

void es_engine_set_edge(es_engine_t* e, uint8_t edge, uint16_t edge_time) {
    es_pattern_t* p = sel(e);
    switch (edge) {
        case ES_EDGE_FIXED:
            if (edge_time < 16) edge_time = 16;
            if (edge_time > 256) edge_time = 256;
            p->edge = ES_EDGE_FIXED;
            p->edge_time = edge_time;
            es_engine_kill_all_notes(e);
            break;
        case ES_EDGE_DRONE: p->edge = ES_EDGE_DRONE; break;
        default:
            p->edge = ES_EDGE_PATTERN;
            es_engine_kill_all_notes(e);
            break;
    }
}

// ---- init / defaults / validation ------------------------------------------

void es_engine_set_defaults(es_config_t* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->arp = 0;
    cfg->p_select = 0;
    cfg->voices = 0xF;
    cfg->octave = 0;
    cfg->scale = 16;  // off
    for (uint8_t i = 0; i < ES_NUM_PATTERNS; i++) {
        es_pattern_t* p = &cfg->p[i];
        p->interval_ind = 0;
        p->length = 0;
        p->loop = 0;
        p->edge = ES_EDGE_PATTERN;
        p->edge_time = 16;
        p->voices = 0xF;
        p->dir = 0;
        p->linearize = 0;
        p->start = 0;
        p->end = 15;
    }
}

bool es_engine_config_valid(const es_config_t* cfg) {
    if (cfg->p_select >= ES_NUM_PATTERNS) return false;
    if (cfg->voices > 0xF) return false;
    if (cfg->octave > 5) return false;
    if (cfg->scale > 16) return false;
    if (cfg->arp > 1) return false;
    for (uint16_t i = 0; i < ES_KEYMAP_SIZE; i++)
        if (cfg->keymap[i] > 2) return false;
    for (uint8_t i = 0; i < ES_NUM_PATTERNS; i++) {
        const es_pattern_t* p = &cfg->p[i];
        if (p->length > ES_EVENTS_PER_PATTERN) return false;
        if (p->interval_ind >= ES_EVENTS_PER_PATTERN) return false;
        if (p->edge > ES_EDGE_DRONE) return false;
        if (p->edge_time > 256) return false;
        if (p->voices > 0xF) return false;
        if (p->dir > 1 || p->linearize > 1 || p->loop > 1) return false;
        for (uint16_t j = 0; j < p->length; j++)
            if (p->e[j].on > 3) return false;
    }
    return true;
}

void es_engine_init(es_engine_t* e, const es_output_t* out) {
    memset(&e->rt, 0, sizeof(e->rt));
    e->rt.mode = es_stopped;
    if (out) e->out = *out;
}
