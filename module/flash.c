#include "flash.h"

#include <string.h>

// asf
#include "flashc.h"
#include "gpio.h"
#include "init_teletype.h"
#include "kria_i2c.h"  // kria_i2c_defaults (follower-bank first-run seed)
#include "music.h"     // SCALE_INT (MP scale-bank defaults)
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
#define FIRSTRUN_KEY 0x2B

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

// First-run seeding helpers. Each holds an ~18 KB staging buffer (scene_state_t
// / kria_config_t). They are kept as separate non-inlined functions so their
// frames never coexist -- inlined into flash_prepare they would sum to ~37 KB
// and overflow the 8 KB stack.
static __attribute__((noinline)) void flash_seed_blank_scenes(void) {
    scene_state_t scene;
    ss_init(&scene);

    char text[SCENE_TEXT_LINES][SCENE_TEXT_CHARS];
    memset(text, 0, SCENE_TEXT_LINES * SCENE_TEXT_CHARS);

    for (uint8_t i = 0; i < SCENE_SLOTS; i++) { flash_write(i, &scene, &text); }
}

static __attribute__((noinline)) void flash_seed_kria_bank(void) {
    kria_config_t kcfg;
    kria_engine_set_defaults(&kcfg);
    flashc_memcpy((void*)&f.kria, &kcfg, sizeof(kcfg), true);
}

void flash_prepare() {
    // if it's not empty return
    if (f.fresh != FIRSTRUN_KEY) {
        int confirm = 1;
        uint32_t counter = 0;
        int toggle = 0;
#define TIMEOUT 100000
        while (confirm == 1 && (++counter < TIMEOUT)) {
            confirm = gpio_get_pin_value(NMI);
            if ((counter % 1000) == 0) {
                if (++toggle % 2)
                    gpio_set_pin_low(B11);
                else
                    gpio_set_pin_high(B11);
            }
            print_dbg_ulong(confirm);
        }
        gpio_set_pin_low(B11);
        if (counter >= TIMEOUT) return;

        print_dbg("\r\n:::: first run, clearing flash");
        print_dbg("\r\nflash size: ");
        print_dbg_ulong(sizeof(f));

        // blank scenes (large stack frame, see helper note above)
        flash_seed_blank_scenes();

        cal_data_t blank_cal_data;
        init_cal_data(&blank_cal_data);
        flashc_memcpy((void*)&f.cal, &blank_cal_data, sizeof(blank_cal_data),
                      true);
        device_config_t device_config = { .flip = 0 };
        flashc_memcpy((void*)&f.device_config, &device_config,
                      sizeof(device_config), true);

        // MP scale bank defaults: 0-6 = the 7 diatonic modes, 7-15 = chromatic
        // (editable). step[0]=0 base; step[1..7]=semitone deltas.
        uint8_t scale_bank[MP_SCALE_SLOTS][8];
        for (uint8_t s = 0; s < MP_SCALE_SLOTS; s++) {
            scale_bank[s][0] = 0;
            for (uint8_t i = 0; i < 7; i++)
                scale_bank[s][i + 1] = (s < 7) ? SCALE_INT[s][i] : 1;
        }
        flashc_memcpy((void*)&f.scale_bank, scale_bank, sizeof(scale_bank),
                      true);

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

        // Kria global song/config bank defaults (large stack frame, see helper)
        flash_seed_kria_bank();

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
    flashc_memcpy((void*)&f.scenes[preset_no].mp, &scene->mp,
                  sizeof(mp_config_t), true);
}

void flash_read(uint8_t preset_no, scene_state_t* scene,
                char (*text)[SCENE_TEXT_LINES][SCENE_TEXT_CHARS],
                uint8_t init_pattern, uint8_t init_grid,
                uint8_t init_i2c_op_address) {
    if (preset_no >= SCENE_SLOTS) return;
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
    memcpy(&scene->mp, &f.scenes[preset_no].mp, sizeof(mp_config_t));
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
