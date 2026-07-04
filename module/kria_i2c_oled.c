#include "kria_i2c_oled.h"

#include <string.h>

#include "font.h"
#include "globals.h"  // region line[8]
#include "keyboard_helper.h"
#include "kria_engine.h"  // KR_F_MO, KR_MIDI_* via kria_i2c.h
#include "kria_i2c.h"
#include "region.h"
#include "util.h"  // itoa
// HID_UP/DOWN/LEFT/RIGHT, HID_ESCAPE, HID_TAB + match_* come via
// keyboard_helper.h

#define OL_LABEL 5
#define OL_VALUE 12
#define OL_CURSOR 15
#define OL_VISIBLE 6  // editable rows (lines 1..6; line 0 header, 7 footer)

// Editable field kinds. `arg` = track / slot index where relevant.
enum {
    F_ACTIVE,
    F_MODE,
    F_OCT,
    F_CHAN,
    F_NOTE0,
    F_PORT,
    F_TRACK,
    F_NOTE,
    F_CHANSLOT
};

typedef struct {
    uint8_t kind;
    uint8_t arg;
} ol_field_t;

static const char* const MODE_NAMES[KR_MIDI_MODE_CT] = {
    "PITCH SGL", "PITCH MUL", "8T NOTES", "8T CHANS"
};

static int8_t ed_index = -1;  // follower being edited (-1 = inactive)
static uint8_t ed_cursor = 0;
static uint8_t ed_scroll = 0;

static ol_field_t fields[32];
static uint8_t field_ct;

// Build the field list for the current follower + mode.
static void build_fields(void) {
    field_ct = 0;
    i2c_follower_t* f = kria_i2c_follower((uint8_t)ed_index);
    uint8_t mode = f->active_mode;

    fields[field_ct++] = (ol_field_t){ F_ACTIVE, 0 };
    fields[field_ct++] = (ol_field_t){ F_MODE, 0 };
    if (mode == KR_MIDI_PITCH_SINGLE || mode == KR_MIDI_PITCH_MULTI)
        fields[field_ct++] = (ol_field_t){ F_OCT, 0 };
    if (mode == KR_MIDI_8T_CHANS)
        fields[field_ct++] = (ol_field_t){ F_NOTE0, 0 };  // shared fixed note
    else
        fields[field_ct++] = (ol_field_t){ F_CHAN, 0 };  // base channel
    if (ed_index == KR_F_MO) fields[field_ct++] = (ol_field_t){ F_PORT, 0 };

    uint8_t ntr =
        (mode >= KR_MIDI_8T_NOTES) ? KRIA_I2C_TRACKS : KRIA_NUM_TRACKS;
    for (uint8_t t = 0; t < ntr; t++)
        fields[field_ct++] = (ol_field_t){ F_TRACK, t };

    if (mode == KR_MIDI_8T_NOTES)
        for (uint8_t s = 0; s < KRIA_I2C_TRACKS; s++)
            fields[field_ct++] = (ol_field_t){ F_NOTE, s };
    else if (mode == KR_MIDI_8T_CHANS)
        for (uint8_t s = 0; s < KRIA_I2C_TRACKS; s++)
            fields[field_ct++] = (ol_field_t){ F_CHANSLOT, s };
}

void kria_i2c_oled_enter(uint8_t index) {
    if (!kria_i2c_is_midi(index)) return;
    ed_index = (int8_t)index;
    ed_cursor = 0;
    ed_scroll = 0;
    build_fields();
}

void kria_i2c_oled_exit(void) {
    ed_index = -1;
}

uint8_t kria_i2c_oled_active(void) {
    return ed_index >= 0;
}

// Format a numbered label like "TRK 3" (prefix + 1-based index).
static void num_label(char* out, const char* prefix, uint8_t n) {
    uint8_t i = 0;
    while (prefix[i]) {
        out[i] = prefix[i];
        i++;
    }
    out[i++] = '1' + n;
    out[i] = 0;
}

static void fmt_field(ol_field_t fld, char* label, char* val) {
    i2c_follower_t* f = kria_i2c_follower((uint8_t)ed_index);
    switch (fld.kind) {
        case F_ACTIVE:
            strcpy(label, "ACTIVE");
            strcpy(val, f->active ? "ON" : "OFF");
            break;
        case F_MODE:
            strcpy(label, "MODE");
            strcpy(val, MODE_NAMES[f->active_mode]);
            break;
        case F_OCT:
            strcpy(label, "OCT");
            itoa(f->oct, val, 10);
            break;
        case F_CHAN:
            strcpy(label, "CHAN");
            itoa(f->chan + 1, val, 10);  // display 1-based
            break;
        case F_NOTE0:
            strcpy(label, "NOTE");
            itoa(f->notes[0], val, 10);
            break;
        case F_PORT:
            strcpy(label, "PORT");
            strcpy(val, f->port ? "B" : "A");
            break;
        case F_TRACK:
            num_label(label, "TRK ", fld.arg);
            strcpy(val, (f->track_en & (1 << fld.arg)) ? "ON" : "OFF");
            break;
        case F_NOTE:
            num_label(label, "NOTE ", fld.arg);
            itoa(f->notes[fld.arg], val, 10);
            break;
        case F_CHANSLOT:
            num_label(label, "CH ", fld.arg);
            itoa(f->chans[fld.arg] + 1, val, 10);  // display 1-based
            break;
        default:
            label[0] = 0;
            val[0] = 0;
            break;
    }
}

// Adjust the focused field by delta (+1 / -1). Rebuilds the field list when the
// mode changes (its dependent fields differ).
static void edit_field(int delta) {
    ol_field_t fld = fields[ed_cursor];
    i2c_follower_t* f = kria_i2c_follower((uint8_t)ed_index);
    uint8_t chmax = kria_i2c_chan_max((uint8_t)ed_index);
    int v;
    switch (fld.kind) {
        case F_ACTIVE: kria_i2c_toggle_active((uint8_t)ed_index); break;
        case F_MODE:
            v = f->active_mode + delta;
            if (v < 0) v = 0;
            if (v >= KR_MIDI_MODE_CT) v = KR_MIDI_MODE_CT - 1;
            kria_i2c_set_mode((uint8_t)ed_index, (uint8_t)v);
            build_fields();
            if (ed_cursor >= field_ct) ed_cursor = field_ct - 1;
            break;
        case F_OCT:
            v = f->oct + delta;
            if (v < -3) v = -3;
            if (v > 3) v = 3;
            kria_i2c_set_octave((uint8_t)ed_index, (int8_t)v);
            break;
        case F_CHAN:
            v = (int)f->chan + delta;
            if (v < 0) v = 0;
            if (v >= chmax) v = chmax - 1;
            kria_i2c_set_channel((uint8_t)ed_index, (uint8_t)v);
            break;
        case F_NOTE0:
            v = (int)f->notes[0] + delta;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            kria_i2c_set_note((uint8_t)ed_index, 0, (uint8_t)v);
            break;
        case F_PORT: kria_i2c_set_port((uint8_t)ed_index, !f->port); break;
        case F_TRACK: kria_i2c_toggle_track((uint8_t)ed_index, fld.arg); break;
        case F_NOTE:
            v = (int)f->notes[fld.arg] + delta;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            kria_i2c_set_note((uint8_t)ed_index, fld.arg, (uint8_t)v);
            break;
        case F_CHANSLOT:
            v = (int)f->chans[fld.arg] + delta;
            if (v < 0) v = 0;
            if (v >= chmax) v = chmax - 1;
            kria_i2c_set_chan_slot((uint8_t)ed_index, fld.arg, (uint8_t)v);
            break;
        default: break;
    }
}

void kria_i2c_oled_render(void) {
    for (uint8_t i = 0; i < 8; i++) region_fill(&line[i], 0);
    if (ed_index < 0) return;
    i2c_follower_t* f = kria_i2c_follower((uint8_t)ed_index);

    font_string_region_clip(&line[0],
                            ed_index == KR_F_MO ? "MO EDIT" : "I2M EDIT", 0, 0,
                            OL_CURSOR, 0);
    font_string_region_clip(&line[0], f->active ? "ON" : "OFF", 104, 0,
                            OL_VALUE, 0);

    // keep the cursor within the visible window
    if (ed_cursor < ed_scroll) ed_scroll = ed_cursor;
    if (ed_cursor >= ed_scroll + OL_VISIBLE)
        ed_scroll = ed_cursor - OL_VISIBLE + 1;

    for (uint8_t r = 0; r < OL_VISIBLE; r++) {
        uint8_t fi = ed_scroll + r;
        if (fi >= field_ct) break;
        char label[16], val[16];
        fmt_field(fields[fi], label, val);
        uint8_t ln = r + 1;
        uint8_t sel = (fi == ed_cursor);
        if (sel) font_string_region_clip(&line[ln], ">", 0, 0, OL_CURSOR, 0);
        font_string_region_clip(&line[ln], label, 8, 0,
                                sel ? OL_CURSOR : OL_LABEL, 0);
        font_string_region_clip(&line[ln], val, 78, 0,
                                sel ? OL_CURSOR : OL_VALUE, 0);
    }

    font_string_region_clip(&line[7], "UP/DN LT/RT EDIT  ENTER/1-4 EXIT", 0, 0,
                            OL_LABEL, 0);
}

// Handle a key while active. Returns 1 only if the editor consumed it; returns
// 0 for anything else so the shell can leave the editor and process the key
// normally (e.g. 1/2/3/4 switch views). NB: <esc>/<tab> are swallowed by the
// module's global key handler and never reach here, so they are NOT exit keys;
// <enter> (not globally bound) is the explicit exit, and any non-editor key
// also exits via the shell fall-through.
uint8_t kria_i2c_oled_key(uint8_t key, uint8_t mod, uint8_t is_held_key) {
    if (ed_index < 0) return 0;
    if (is_held_key) return 0;

    if (match_no_mod(mod, key, HID_ENTER)) {
        kria_i2c_oled_exit();  // explicit exit back to the i2c view
        return 1;
    }
    if (match_no_mod(mod, key, HID_UP)) {
        if (ed_cursor) ed_cursor--;
    }
    else if (match_no_mod(mod, key, HID_DOWN)) {
        if (ed_cursor + 1 < field_ct) ed_cursor++;
    }
    else if (match_no_mod(mod, key, HID_LEFT)) { edit_field(-1); }
    else if (match_no_mod(mod, key, HID_RIGHT)) { edit_field(+1); }
    else {
        return 0;  // not ours -> let the shell exit the editor + handle it
    }
    return 1;
}
