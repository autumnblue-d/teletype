#include "flash.h"

#include <string.h>

// asf
#include "flashc.h"
#include "gpio.h"
#include "init_teletype.h"
#include "kria_i2c.h"  // kria_i2c_defaults (follower-bank first-run seed)
#include "music.h"     // SCALE_INT (MP scale-bank defaults)
#include "ops/midi.h"  // mo_flush_note_offs (release held MO notes on load)
#include "print_funcs.h"

// this
#include "teletype.h"

// Bumped 0x22 -> 0x23 for the Meadowphysics port (SCENE_SLOTS 32->30 +
// per-scene mp_config_t), then -> 0x24 for the global MP scale bank in
// nvram_data_t. Each layout change forces a flash reinit on upgrade (no
// per-scene migration path).
// 0x24 -> 0x25: Kria (SCENE_SLOTS 30->20 + global kria_config_t bank);
// -> 0x26/0x27 for the i2c follower bits; -> 0x28 for the global i2c bank.
// -> 0x29: I2M + MO MIDI followers (6->8 followers + MIDI fields in fstate).
// -> 0x2A: Earthsea (SCENE_SLOTS 20->18 + global es_config_t bank).
// -> 0x2B: force reformat so f.kria / f.kria_i2c get seeded at first-run
// (previously only repaired by boot-time fallbacks).
// -> 0x2C: SCENE_SLOTS 18->16.
// -> 0x2D: MP moved from a per-scene mp_config_t to a global 8-slot preset bank
// (f.mp_slots + f.mp_current); nvram_scene_t.mp dropped.
// -> 0x2E: global per-output CV tuning bank (f.tuning_table + f.tuning_fresh).
// -> 0x2F: kria/MP/ES global banks added to nvram_data_t.
// -> 0x30: force a clean reseed (kria/MP/ES layout).
// -> 0x31: Kria per-pattern script-trigger sequencer (script_* fields in
// kria_pattern_t; per-lane loop/divider arrays enlarge kria_config_t).
#define FIRSTRUN_KEY 0x31

// Independent version tag for the global scale bank. The scale bank has no
// load-time validity check (unlike the kria/mp configs, which self-repair via
// *_config_valid), so a stale or never-seeded bank is used verbatim and shows
// up as garbage default scales. flash_prepare() reseeds it on mismatch, which
// self-heals on upgrade WITHOUT the scene-wiping full first-run. Bump when the
// default scales change.
#define SCALE_BANK_KEY 0x01

// Independent version tag for the global tuning bank, same self-heal rationale
// as SCALE_BANK_KEY: the bank is used verbatim from flash (no validity check),
// so seed/refresh it whenever the tag mismatches. Bump when the default table
// (equal temperament) changes.
#define TUNING_BANK_KEY 0x01

static grid_data_t grid_data;

#if defined(__AVR32__)
static __attribute__((__section__(".flash_nvram"))) nvram_data_t f;
#else
nvram_data_t f;
#endif

static void pack_grid(scene_state_t* scene);
static void unpack_grid(scene_state_t* scene);

u8 is_flash_fresh() {
    return f.fresh != FIRSTRUN_KEY;
}

// Seed all scene slots blank. A scene_state_t is ~18 KB, which overflows the
// 8 KB stack (hard fault), so the caller passes in an existing buffer to reuse
// -- main.c hands us its live `scene_state` global (reinitialised here; main.c
// reloads it from flash right after flash_prepare(), so this is harmless). The
// small text buffer stays on the stack.
static void flash_seed_blank_scenes(scene_state_t* scene) {
    ss_init(scene);

    char text[SCENE_TEXT_LINES][SCENE_TEXT_CHARS];
    memset(text, 0, SCENE_TEXT_LINES * SCENE_TEXT_CHARS);

    for (uint8_t i = 0; i < SCENE_SLOTS; i++) { flash_write(i, scene, &text); }
}

// (No first-run seeding for the Kria bank: kria_config_t is ~20 KB and would
// overflow the stack, and it is unnecessary -- km_init_once() self-repairs via
// kria_engine_config_valid()/kria_engine_set_defaults() when the stored config
// is invalid, e.g. on a fresh flash. Same self-heal applies to the MP bank.)

// MP global preset bank defaults: every slot gets the default config and a
// blank glyph; the current slot is 0. Written slot-by-slot so the staging
// buffer stays a single mp_config_t rather than the whole bank.
static __attribute__((noinline)) void flash_seed_mp_bank(void) {
    mp_config_t cfg;
    mp_engine_set_defaults(&cfg);
    for (uint8_t s = 0; s < MP_SLOTS; s++) {
        flashc_memcpy((void*)&f.mp_slots[s].cfg, &cfg, sizeof(cfg), true);
        flashc_memset8((void*)&f.mp_slots[s].glyph, 0, 8, true);
    }
    flashc_memset8((void*)&f.mp_current, 0, 1, true);
}

// Global scale bank defaults: slots 0-6 = the 7 diatonic modes, 7-15 =
// chromatic (editable). step[0]=0 base; step[1..7]=semitone deltas, matching
// Ansible's default_kria/default_mp seeding.
static void flash_seed_scale_bank(void) {
    uint8_t scale_bank[MP_SCALE_SLOTS][8];
    for (uint8_t s = 0; s < MP_SCALE_SLOTS; s++) {
        scale_bank[s][0] = 0;
        for (uint8_t i = 0; i < 7; i++)
            scale_bank[s][i + 1] = (s < 7) ? SCALE_INT[s][i] : 1;
    }
    flashc_memcpy((void*)&f.scale_bank, scale_bank, sizeof(scale_bank), true);
}

// Global tuning bank default: equal temperament on every channel.
// tuning_default fills the RAM working copy (a static global, no stack cost);
// copy it to flash. main.c reloads the RAM copy from flash after
// flash_prepare, so mutating it here is harmless.
static void flash_seed_tuning(void) {
    tuning_default();
    flashc_memcpy((void*)&f.tuning_table, tuning_table, sizeof(f.tuning_table),
                  true);
}

void flash_prepare(scene_state_t* scene) {
    // if it's not empty return
    if (f.fresh != FIRSTRUN_KEY) {
        // Fresh flash: FIRSTRUN_KEY changed, so the persisted layout changed and
        // the existing scenes are already incompatible -> reseed unconditionally.
        //
        // This previously busy-waited for a front-panel (NMI) press to confirm
        // the wipe, calling print_dbg_ulong() every iteration. With nothing
        // draining the debug UART that call blocks on a full TX buffer, so the
        // loop could stall *inside* the print and never re-sample the button: the
        // confirmation screen stayed up, a front-panel press did nothing, and the
        // key was never written -> every boot re-prompted. A key bump already
        // implies a wipe, so just reseed.
        print_dbg("\r\n:::: first run, clearing flash");
        print_dbg("\r\nflash size: ");
        print_dbg_ulong(sizeof(f));

        // blank scenes (large stack frame, see helper note above)
        flash_seed_blank_scenes(scene);

        cal_data_t blank_cal_data;
        init_cal_data(&blank_cal_data);
        flashc_memcpy((void*)&f.cal, &blank_cal_data, sizeof(blank_cal_data),
                      true);
        device_config_t device_config = { .flip = 0 };
        flashc_memcpy((void*)&f.device_config, &device_config,
                      sizeof(device_config), true);

        // (the global scale bank is seeded by the version-tagged self-heal
        // below, which also refreshes it on upgrade without a full first-run)

        // Earthsea bank defaults (single global instance), written piecewise:
        // a full es_config_t staging buffer would cost 8.6 KB of RAM, and the
        // heap barely fits the OLED line regions as it is (blank-screen bug).
        // Must stay equivalent to es_engine_set_defaults(): zeros everywhere
        // except voices=0xF, scale=16 (off) and per-pattern edge_time=16,
        // voices=0xF, end=15.
        flashc_memset8((void*)&f.earthsea, 0, sizeof(es_config_t), true);
        flashc_memset8((void*)&f.earthsea.voices, 0xF, 1, true);
        flashc_memset8((void*)&f.earthsea.scale, 16, 1, true);
        es_pattern_t es_pattern_default;
        memset(&es_pattern_default, 0, sizeof(es_pattern_default));
        es_pattern_default.edge_time = 16;
        es_pattern_default.voices = 0xF;
        es_pattern_default.end = 15;
        for (uint8_t p = 0; p < ES_NUM_PATTERNS; p++)
            flashc_memcpy((void*)&f.earthsea.p[p], &es_pattern_default,
                          sizeof(es_pattern_default), true);

        // (Kria bank is not seeded here: it self-repairs on load -- see the note
        // by flash_seed_blank_scenes -- and a full kria_config_t staging buffer
        // would overflow the stack.)

        // MP global 8-slot preset bank defaults (blank glyphs, current = 0)
        flash_seed_mp_bank();

        // Global i2c follower bank defaults (shared by Kria + MP).
        {
            kria_i2c_fstate_t idef[KRIA_I2C_FOLLOWERS];
            kria_i2c_defaults(idef);
            flashc_memcpy((void*)&f.kria_i2c, idef, sizeof(idef), true);
        }

        flash_update_last_saved_scene(0);
        flash_update_last_mode(M_LIVE);
        flashc_memset8((void*)&f.fresh, FIRSTRUN_KEY, 1, true);
    }

    // Self-heal the global scale bank independently of the scene-wiping first
    // run above. Because the bank is used verbatim from flash (no *_config_valid
    // check like kria/mp), a stale or never-seeded bank surfaces as garbage
    // default scales. Reseed from defaults whenever the version tag mismatches:
    // this runs on every boot but only writes flash once per version bump, and
    // never touches scenes or the kria song. Covers upgrades and any device
    // whose bank predates SCALE_BANK_KEY.
    if (f.scale_fresh != SCALE_BANK_KEY) {
        flash_seed_scale_bank();
        flashc_memset8((void*)&f.scale_fresh, SCALE_BANK_KEY, 1, true);
    }

    // Same version-tagged self-heal for the global tuning bank (seeds it on a
    // fresh device and on upgrade without wiping scenes).
    if (f.tuning_fresh != TUNING_BANK_KEY) {
        flash_seed_tuning();
        flashc_memset8((void*)&f.tuning_fresh, TUNING_BANK_KEY, 1, true);
    }
}

void flash_write(uint8_t preset_no, scene_state_t* scene,
                 char (*text)[SCENE_TEXT_LINES][SCENE_TEXT_CHARS]) {
    if (preset_no >= SCENE_SLOTS) return;
    flashc_memcpy((void*)&f.scenes[preset_no].scripts, ss_scripts_ptr(scene),
                  ss_scripts_size(EDITABLE_SCRIPT_COUNT), true);
    flashc_memcpy((void*)&f.scenes[preset_no].patterns, ss_patterns_ptr(scene),
                  ss_patterns_size(), true);
    pack_grid(scene);
    flashc_memcpy((void*)&f.scenes[preset_no].grid_data, &grid_data,
                  sizeof(grid_data_t), true);
    flashc_memcpy((void*)&f.scenes[preset_no].text, text,
                  SCENE_TEXT_LINES * SCENE_TEXT_CHARS, true);
}

void flash_read(uint8_t preset_no, scene_state_t* scene,
                char (*text)[SCENE_TEXT_LINES][SCENE_TEXT_CHARS],
                uint8_t init_pattern, uint8_t init_grid,
                uint8_t init_i2c_op_address) {
    if (preset_no >= SCENE_SLOTS) return;
    // release any notes the outgoing scene was holding so they don't hang
    mo_flush_note_offs(scene);
    memcpy(ss_scripts_ptr(scene), &f.scenes[preset_no].scripts,
           ss_scripts_size(EDITABLE_SCRIPT_COUNT));
    if (init_pattern) {
        memcpy(ss_patterns_ptr(scene), &f.scenes[preset_no].patterns,
               ss_patterns_size());
    }
    if (init_grid) {
        memcpy(&grid_data, &f.scenes[preset_no].grid_data, sizeof(grid_data_t));
        unpack_grid(scene);
    }
    memcpy(text, &f.scenes[preset_no].text,
           SCENE_TEXT_LINES * SCENE_TEXT_CHARS);
    // need to reset timestamps
    uint32_t ticks = get_ticks();
    for (size_t i = 0; i < TOTAL_SCRIPT_COUNT; i++)
        scene->scripts[i].last_time = ticks;
    scene->variables.time = 0;

    if (init_i2c_op_address) scene->i2c_op_address = -1;
    ss_midi_init(scene);
}

uint8_t flash_last_saved_scene() {
    return f.last_scene;
}

void flash_update_last_saved_scene(uint8_t preset_no) {
    if (preset_no >= SCENE_SLOTS) return;
    flashc_memset8((void*)&f.last_scene, preset_no, 1, true);
}

const char* flash_scene_text(uint8_t preset_no, size_t line) {
    return f.scenes[preset_no].text[line];
}

tele_mode_t flash_last_mode() {
    return f.last_mode;
}

void flash_update_last_mode(tele_mode_t mode) {
    // flashc_memset8((void *)&f.last_mode, mode, sizeof(tele_mode_t), true);
}

void flash_update_cal(cal_data_t* cal) {
    flashc_memcpy((void*)&f.cal, cal, sizeof(cal_data_t), true);
}

void flash_get_cal(cal_data_t* cal) {
    *cal = f.cal;
}

void flash_get_scale_bank(uint8_t (*bank)[8]) {
    memcpy(bank, f.scale_bank, sizeof(f.scale_bank));
}

void flash_update_scale_bank(uint8_t (*bank)[8]) {
    flashc_memcpy((void*)&f.scale_bank, bank, sizeof(f.scale_bank), true);
}

void flash_get_tuning(uint16_t (*table)[TUNING_SLOTS]) {
    memcpy(table, f.tuning_table, sizeof(f.tuning_table));
}

void flash_update_tuning(uint16_t (*table)[TUNING_SLOTS]) {
    flashc_memcpy((void*)&f.tuning_table, table, sizeof(f.tuning_table), true);
}

void flash_get_kria(kria_config_t* dst) {
    memcpy(dst, &f.kria, sizeof(f.kria));
}

void flash_update_kria(const kria_config_t* src) {
    flashc_memcpy((void*)&f.kria, src, sizeof(f.kria), true);
}

void flash_get_kria_i2c(kria_i2c_fstate_t* dst) {
    memcpy(dst, f.kria_i2c, sizeof(f.kria_i2c));
}

void flash_update_kria_i2c(const kria_i2c_fstate_t* src) {
    flashc_memcpy((void*)&f.kria_i2c, src, sizeof(f.kria_i2c), true);
}

void flash_get_es(es_config_t* dst) {
    memcpy(dst, &f.earthsea, sizeof(f.earthsea));
}

void flash_update_es(const es_config_t* src) {
    flashc_memcpy((void*)&f.earthsea, src, sizeof(f.earthsea), true);
}

void flash_get_mp_slot(uint8_t slot, mp_config_t* cfg, uint8_t glyph[8]) {
    if (slot >= MP_SLOTS) return;
    memcpy(cfg, &f.mp_slots[slot].cfg, sizeof(mp_config_t));
    memcpy(glyph, f.mp_slots[slot].glyph, 8);
}

void flash_update_mp_slot(uint8_t slot, const mp_config_t* cfg,
                          const uint8_t glyph[8]) {
    if (slot >= MP_SLOTS) return;
    flashc_memcpy((void*)&f.mp_slots[slot].cfg, cfg, sizeof(mp_config_t), true);
    flashc_memcpy((void*)&f.mp_slots[slot].glyph, glyph, 8, true);
}

uint8_t flash_get_mp_current(void) {
    return f.mp_current < MP_SLOTS ? f.mp_current : 0;
}

void flash_update_mp_current(uint8_t slot) {
    if (slot >= MP_SLOTS) return;
    flashc_memset8((void*)&f.mp_current, slot, 1, true);
}

void flash_update_device_config(device_config_t* device_config) {
    flashc_memcpy((void*)&f.device_config, device_config,
                  sizeof(device_config_t), true);
}

void flash_get_device_config(device_config_t* device_config) {
    *device_config = f.device_config;
}

static void pack_grid(scene_state_t* scene) {
    uint8_t byte = 0;
    uint8_t byte_count = 0;
    for (uint16_t i = 0; i < GRID_BUTTON_COUNT; i++) {
        byte |= (scene->grid.button[i].state != 0) << (i & 7);
        if ((i & 7) == 7) {
            grid_data.button_states[byte_count] = byte;
            byte = 0;
            if (++byte_count >= BUTTON_STATE_SIZE) break;
        }
    }
    for (uint16_t i = 0; i < GRID_FADER_COUNT; i++)
        grid_data.fader_states[i] = scene->grid.fader[i].value;
}

static void unpack_grid(scene_state_t* scene) {
    for (uint16_t i = 0; i < GRID_BUTTON_COUNT; i++) {
        scene->grid.button[i].state =
            0 != (grid_data.button_states[i >> 3] & (1 << (i & 7)));
    }
    for (uint16_t i = 0; i < GRID_FADER_COUNT; i++)
        scene->grid.fader[i].value = grid_data.fader_states[i];
}
