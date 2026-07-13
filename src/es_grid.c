// Earthsea grid surface -- see es_grid.h. Faithful port of Ansible's
// handler_ESGridKey / refresh_es, minus the 256-grid and preset-screen
// branches. Cell coordinates in comments refer to Ansible's flat
// monomeLedBuffer indices (y*16+x).

#include "es_grid.h"

#include <string.h>

#include "grid_led.h"    // grid_led_finalize
#include "scale_util.h"  // cumulative_scale

// ---- held-key bitmap ------------------------------------------------------

static void held_set(es_grid_state_t* g, uint8_t idx, uint8_t z) {
    if (idx >= ES_KEYMAP_SIZE) return;
    if (z)
        g->held[idx >> 3] |= (uint8_t)(1 << (idx & 7));
    else
        g->held[idx >> 3] &= (uint8_t) ~(1 << (idx & 7));
}

static uint8_t held_get(const es_grid_state_t* g, uint8_t idx) {
    if (idx >= ES_KEYMAP_SIZE) return 0;
    return (g->held[idx >> 3] >> (idx & 7)) & 1;
}

// Ansible is_arm_pressed(): the arm key is (0,2) = index 32.
static uint8_t arm_pressed(const es_grid_state_t* g) {
    return held_get(g, 32);
}

// Ansible rest_pressed(): the rest key is (15,0) = index 15.
static uint8_t rest_pressed(const es_grid_state_t* g) {
    return held_get(g, 15);
}

void es_grid_state_init(es_grid_state_t* g) {
    memset(g, 0, sizeof(*g));
    g->view = ES_VIEW_MAIN;
}

// ---- key handling ----------------------------------------------------------

static uint32_t control_column_key(es_engine_t* e, es_grid_state_t* g,
                                   uint8_t y, uint8_t z, uint32_t now) {
    es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    uint32_t arm_interval = 0;

    if (z && y == 0) {  // start/stop
        if (g->view == ES_VIEW_PATTERNS_HELD) { g->view = ES_VIEW_PATTERNS; }
        else if (e->rt.mode == es_stopped || e->rt.mode == es_armed) {
            arm_interval = es_engine_start_playback(e, 0, now);
            if (arm_pressed(g)) g->ignore_arm_release = 1;
        }
        else if (e->rt.mode == es_recording) {
            p->loop = 1;
            arm_interval = es_engine_start_playback(e, 0, now);
        }
        else if (e->rt.mode == es_playing) {
            if (arm_pressed(g)) {
                arm_interval = es_engine_start_playback(e, 0, now);
                g->ignore_arm_release = 1;
            }
            else
                es_engine_stop_playback(e);
        }
    }
    else if (y == 1) {  // pattern view
        if (z && e->rt.mode == es_recording) es_engine_stop_recording(e, now);
        if (z && g->view == ES_VIEW_PATTERNS)
            g->view = ES_VIEW_MAIN;
        else if (z && g->view == ES_VIEW_MAIN)
            g->view = ES_VIEW_PATTERNS_HELD;
        else if (!z && g->view == ES_VIEW_PATTERNS_HELD)
            g->view = ES_VIEW_MAIN;
    }
    else if (y == 2) {  // arm
        g->view = ES_VIEW_MAIN;
        if (z) {
            if (e->rt.mode == es_armed) {
                e->rt.mode = es_stopped;
                g->ignore_arm_release = 1;
            }
            else if (e->rt.mode == es_recording) {
                es_engine_stop_recording(e, now);
                g->ignore_arm_release = 1;
            }
        }
        else {
            if (g->ignore_arm_release) {
                g->ignore_arm_release = 0;
                return 0;
            }
            if (e->rt.mode == es_stopped || e->rt.mode == es_playing)
                es_engine_arm(e);
        }
    }
    else if (z && y == 3) {  // loop
        p->loop = !p->loop;
    }
    else if (z && y == 4) {  // arp
        e->cfg.arp = !e->cfg.arp;
    }
    else if (y == 5) {  // edge view (momentary)
        g->edge_held = z;
    }
    else if (y == 6) {  // runes (momentary)
        g->runes_held = z;
    }
    else if (y == 7) {  // voices (momentary)
        g->voices_held = z;
    }

    return arm_interval;
}

static void runes_key(es_engine_t* e, uint8_t x, uint8_t y) {
    if (x > 1 && x < 5 && y > 1 && y < 5) {
        es_engine_set_linearize(e, !e->cfg.p[e->cfg.p_select].linearize);
    }
    else if (x > 5 && x < 8 && y > 1 && y < 5) {
        es_engine_set_direction(e, 1);
    }
    else if (x > 8 && x < 11 && y > 1 && y < 5) {
        es_engine_set_direction(e, 0);
    }
    else if (x > 11 && x < 15 && y > 0 && y < 3) { es_engine_double_speed(e); }
    else if (x > 11 && x < 15 && y > 3 && y < 6) { es_engine_half_speed(e); }
}

static void edge_key(es_engine_t* e, uint8_t x, uint8_t y) {
    es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    if (y == 7) { es_engine_set_edge(e, ES_EDGE_FIXED, (x + 1) << 4); }
    else if (x) {
        if (x < 6)
            es_engine_set_edge(e, ES_EDGE_PATTERN, 0);
        else if (x < 11)
            es_engine_set_edge(e, ES_EDGE_FIXED, p->edge_time);
        else
            es_engine_set_edge(e, ES_EDGE_DRONE, 0);
    }
}

static void voices_key(es_engine_t* e, uint8_t x, uint8_t y) {
    es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    uint8_t voice = (uint8_t)(1 << (y - 2));
    if (x == 3 && y > 1 && y < 6) { e->cfg.voices ^= voice; }
    else if (x == 2 && y > 1 && y < 6) { p->voices ^= voice; }
    else if (y == 7 && x == 2 && e->cfg.octave) {
        e->cfg.octave--;  // stored/displayed only; inert upstream too
    }
    else if (y == 7 && x == 3 && e->cfg.octave < 5) { e->cfg.octave++; }
    es_engine_kill_all_notes(e);
}

static uint32_t patterns_key(es_engine_t* e, es_grid_state_t* g, uint8_t x,
                             uint8_t y, uint32_t now) {
    if (x > 7 && y > 2 && y < 5) {
        // scale display select (toggle off by re-pressing)
        uint8_t scale = (uint8_t)(x - 8 + ((y - 3) << 3));
        if (scale == e->cfg.scale)
            e->cfg.scale = 16;
        else
            e->cfg.scale = scale;
        return 0;
    }

    if (x < 2 || x > 5 || y < 2 || y > 5) return 0;
    es_engine_set_pattern(e, (uint8_t)((x - 2) + ((y - 2) << 2)));
    if (g->view == ES_VIEW_PATTERNS) return es_engine_start_playback(e, 0, now);
    return 0;
}

uint32_t es_grid_process_key(es_engine_t* e, es_grid_state_t* g, uint8_t x,
                             uint8_t y, uint8_t z, uint32_t now) {
    if (x > 15 || y > 7) return 0;

    held_set(g, (uint8_t)((y << 4) + x), z);

    if (x == 0) return control_column_key(e, g, y, z, now);

    // Momentary overlay views consume everything (presses only). Priority
    // mirrors refresh_es: runes > edge > voices.
    if (g->runes_held) {
        if (z) runes_key(e, x, y);
        return 0;
    }
    if (g->edge_held) {
        if (z) edge_key(e, x, y);
        return 0;
    }
    if (g->voices_held) {
        if (z) voices_key(e, x, y);
        return 0;
    }

    if (g->view == ES_VIEW_PATTERNS_HELD || g->view == ES_VIEW_PATTERNS) {
        if (!z) return 0;
        return patterns_key(e, g, x, y, now);
    }

    if (y == 0 && e->rt.mode == es_playing) {
        // scrub row: restart at pos/16 (mirrored when reversed)
        if (!z) return 0;
        return es_engine_start_playback(
            e, e->cfg.p[e->cfg.p_select].dir ? 15 - x : x, now);
    }

    // playable field (armed/recording/arp/keymap/live handled by the engine)
    return es_engine_grid_press(e, x, y, z, rest_pressed(g), now);
}

// ---- rendering -------------------------------------------------------------

#define L(xx, yy) led[((yy) << 4) + (xx)]

static void render_runes(es_engine_t* e, uint8_t* led) {
    const es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    uint8_t l;

    l = p->linearize ? 15 : 7;  // linearize rune (2x2 dots)
    L(2, 2) = l;
    L(4, 2) = l;
    L(2, 4) = l;
    L(4, 4) = l;

    l = p->dir ? 15 : 7;  // reverse arrow
    L(7, 2) = l;
    L(6, 3) = l;
    L(7, 4) = l;

    l = p->dir ? 7 : 15;  // forward arrow
    L(9, 2) = l;
    L(10, 3) = l;
    L(9, 4) = l;

    l = 8;  // double speed
    L(13, 1) = l;
    L(12, 2) = l;
    L(14, 2) = l;

    // half speed
    L(12, 4) = l;
    L(14, 4) = l;
    L(13, 5) = l;
}

static void render_edge(es_engine_t* e, uint8_t* led) {
    const es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    uint8_t l;

    l = p->edge == ES_EDGE_PATTERN ? 15 : 7;  // "N" glyph
    L(2, 2) = l;
    L(3, 2) = l;
    L(4, 2) = l;
    L(2, 3) = l;
    L(4, 3) = l;
    L(2, 4) = l;
    L(4, 4) = l;
    L(2, 5) = l;
    L(4, 5) = l;
    L(5, 5) = l;

    l = p->edge == ES_EDGE_FIXED ? 15 : 7;  // "F" glyph
    L(7, 2) = l;
    L(8, 2) = l;
    L(9, 2) = l;
    L(10, 2) = l;
    L(7, 3) = l;
    L(10, 3) = l;
    L(7, 4) = l;
    L(10, 4) = l;
    L(7, 5) = l;
    L(10, 5) = l;

    l = p->edge == ES_EDGE_DRONE ? 15 : 7;  // "D" glyph
    L(12, 2) = l;
    L(13, 2) = l;
    L(14, 2) = l;
    L(15, 2) = l;

    if (p->edge == ES_EDGE_FIXED) {
        for (uint8_t i = 0; i < 16; i++) L(i, 7) = 4;
        uint8_t ei = (uint8_t)(p->edge_time >> 4);
        if (ei >= 1 && ei <= 16) L(ei - 1, 7) = 11;
    }
}

static void render_voices(es_engine_t* e, uint8_t* led) {
    const es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    for (uint8_t i = 0; i < ES_NUM_VOICES; i++) {
        L(3, 2 + i) = (e->cfg.voices & (1 << i)) ? 15 : 4;
        L(2, 2 + i) = (p->voices & (1 << i)) ? 15 : 4;
    }
    L(e->cfg.octave ? 3 : 2, 7) = (uint8_t)(10 + e->cfg.octave);
}

static void render_patterns(es_engine_t* e, uint8_t* led) {
    for (uint8_t i = 0; i < ES_NUM_PATTERNS; i++)
        L(2 + (i & 3), 2 + (i >> 2)) = e->cfg.p[i].length ? 7 : 4;
    L(2 + (e->cfg.p_select & 3), 2 + (e->cfg.p_select >> 2)) = 15;

    // scale strip
    for (uint8_t x = 8; x < 16; x++)
        for (uint8_t y = 3; y < 5; y++) L(x, y) = 4;
    if (e->cfg.scale != 16)
        L(8 + (e->cfg.scale & 7), 3 + (e->cfg.scale >> 3)) = 15;
}

static void render_main(es_engine_t* e, es_grid_state_t* g, uint8_t* led) {
    const es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    uint8_t ystart = (e->rt.mode == es_playing) ? 1 : 0;

    if (e->cfg.scale == 16) {
        // keymap shading
        for (uint8_t x = 1; x < 16; x++)
            for (uint8_t y = ystart; y < 8; y++) {
                uint8_t i = (uint8_t)((y << 4) + x);
                if (e->cfg.keymap[i])
                    L(x, y) = (uint8_t)(e->cfg.keymap[i] << 1);
            }
    }
    else if (g->scale_bank) {
        // scale overlay: light in-scale semitones (root brighter). Cumulative
        // scale from the shared bank, matching Ansible calc_scale/cur_scale.
        uint8_t cur[8];
        cumulative_scale(cur, g->scale_bank[e->cfg.scale]);

        for (uint8_t x = 1; x < 16; x++)
            for (uint8_t y = ystart; y < 8; y++) {
                int16_t index = es_engine_note_index((int8_t)x, (int8_t)y);
                for (uint8_t sc = 0; sc < 8; sc++) {
                    uint8_t in_scale = 0;
                    for (uint8_t oct = 0; oct < 9; oct++) {
                        if (index == cur[sc] + oct * 12) {
                            L(x, y) = sc == 0 ? 4 : 2;
                            in_scale = 1;
                            break;
                        }
                    }
                    if (in_scale) break;
                }
            }
    }

    if (e->cfg.arp) L(p->root_x & 15, p->root_y & 7) = 7;

    // active notes (wrap arp-shifted coords back onto the grid by fourths)
    for (uint8_t v = 0; v < ES_NUM_VOICES; v++) {
        if (!e->rt.notes[v].active) continue;
        int16_t x = e->rt.notes[v].x;
        int16_t y = e->rt.notes[v].y;
        while (x < 0) {
            y++;
            x += 5;
        }
        while (x > 15) {
            y--;
            x -= 5;
        }
        if (y >= 0 && y < 8 && x != 0) L(x, y) = 15;
    }
}

void es_grid_refresh(es_engine_t* e, es_grid_state_t* g, uint8_t* led,
                     uint8_t vari, uint32_t now) {
    const es_pattern_t* p = &e->cfg.p[e->cfg.p_select];
    memset(led, 0, ES_KEYMAP_SIZE);

    // control column
    for (uint8_t y = 0; y < 8; y++) L(0, y) = 2;

    if (e->rt.mode == es_playing)
        L(0, 0) = 15;
    else if (p->length)
        L(0, 0) = 8;

    if (g->view == ES_VIEW_PATTERNS) L(0, 1) = 15;

    if (e->rt.mode == es_recording)
        L(0, 2) = (uint8_t)(11 + (g->blinker ? 0 : 4));
    else if (e->rt.mode == es_armed)
        L(0, 2) = 7;

    if (p->loop) L(0, 3) = 11;
    if (e->cfg.arp) L(0, 4) = 11;

    // playback position bar (row 0)
    if (e->rt.mode == es_playing) {
        uint8_t pos = 0;
        if (e->rt.clock_external) {
            if (p->length > 1)
                pos = (uint8_t)((e->rt.pos << 4) / (p->length - 1));
        }
        else if (e->rt.p_total) {
            uint32_t el = ((now - e->rt.p_start) << 4) / e->rt.p_total;
            pos = el > 15 ? 15 : (uint8_t)el;
        }
        if (p->dir) pos = 15 - pos;
        for (uint8_t i = 1; i < 16; i++)
            if (i <= pos) L(i, 0) = 8;
    }

    // exclusive views (priority mirrors Ansible refresh_es on 8 rows)
    uint8_t show_patterns =
        g->view == ES_VIEW_PATTERNS || g->view == ES_VIEW_PATTERNS_HELD;
    uint8_t show_edge = g->edge_held && !g->runes_held;
    uint8_t show_voices = g->voices_held && !(g->runes_held || g->edge_held);

    if (g->runes_held) { render_runes(e, led); }
    else if (show_edge) { render_edge(e, led); }
    else if (show_voices) { render_voices(e, led); }
    else if (show_patterns) { render_patterns(e, led); }
    else { render_main(e, g, led); }

    grid_led_finalize(led, vari);
}
