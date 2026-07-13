#ifndef _FLASH_H_
#define _FLASH_H_

#include <stdint.h>

#include "es_engine.h"
#include "globals.h"
#include "kria_engine.h"
#include "line_editor.h"
#include "teletype.h"
#include "tuning.h"  // TUNING_CHANNELS / TUNING_SLOTS

// Reduced from 32 to 30 to reclaim ~12 KB of NVRAM: this lets the flash NVRAM
// region shrink (see __flash_nvram_size__ in config.mk), freeing program flash
// for the Meadowphysics mode, while still leaving room for MP's per-scene data
// (Phase 7). See MEADOWPHYSICS_PORT_PLAN.md.
// Kria (Scenario B): reduced 30 -> 20. Dropping 10 scenes (~63 KB) funds the
// 16-pattern global Kria bank (~18.5 KB) and leaves ~49 KB of program flash for
// Kria code. See KRIA_PORT_PLAN.md §0.
// Earthsea (Scenario A): reduced 20 -> 18. Dropping 2 scenes (~12.7 KB) funds
// the 16-pattern global Earthsea bank (~8.6 KB). See EARTHSEA_PORT_PLAN.md §0.
#define SCENE_SLOTS 16
#define BUTTON_STATE_SIZE (GRID_BUTTON_COUNT >> 3)

typedef struct {
    uint8_t button_states[BUTTON_STATE_SIZE];
    uint8_t fader_states[GRID_FADER_COUNT];
} grid_data_t;

// NVRAM data structure located in the flash array.
typedef struct {
    scene_script_t scripts[EDITABLE_SCRIPT_COUNT];  // Exclude TEMP script
    scene_pattern_t patterns[PATTERN_COUNT];
    grid_data_t grid_data;
    char text[SCENE_TEXT_LINES][SCENE_TEXT_CHARS];
} nvram_scene_t;

// Meadowphysics global 8-slot preset bank (ansible-style): each slot holds a
// full mp_config_t plus a drawable 8x8 glyph (glyph[row] is a bitmask of the 8
// columns). Replaces the old per-scene mp_config_t; MP is now scene-independent
// like the Kria/Earthsea banks.
#define MP_SLOTS 8
typedef struct {
    mp_config_t cfg;
    uint8_t glyph[8];
} mp_slot_t;

// Meadowphysics global editable scale bank (MP_SCALE_SLOTS x 8, from
// meadowphysics_engine.h).
typedef struct {
    nvram_scene_t scenes[SCENE_SLOTS];
    uint8_t last_scene;
    tele_mode_t last_mode;
    uint8_t fresh;
    cal_data_t cal;
    device_config_t device_config;
    uint8_t scale_bank[MP_SCALE_SLOTS][8];
    kria_config_t kria;  // Kria global preset bank (single song), not per-scene
    kria_i2c_fstate_t kria_i2c[KRIA_I2C_FOLLOWERS];  // global i2c follower bank
    es_config_t earthsea;          // Earthsea global bank (single instance)
    mp_slot_t mp_slots[MP_SLOTS];  // Meadowphysics global 8-slot preset bank
    uint8_t mp_current;            // last-used MP slot, reloaded on boot
    uint8_t scale_fresh;           // version tag for the scale-bank self-heal
                                   // (see SCALE_BANK_KEY in flash.c)
    // Global per-output CV tuning bank (Ansible "tuning" port). Edited by the
    // Kria grid tuning view, used by all ported grid apps' module CV out.
    uint16_t tuning_table[TUNING_CHANNELS][TUNING_SLOTS];
    uint8_t tuning_fresh;  // version tag for the tuning-bank self-heal (see
                           // TUNING_BANK_KEY); kept last so adding it doesn't
                           // shift any existing field's flash offset
} nvram_data_t;

u8 is_flash_fresh(void);
void flash_prepare(void);
void flash_read(uint8_t preset_no, scene_state_t* scene,
                char (*text)[SCENE_TEXT_LINES][SCENE_TEXT_CHARS],
                uint8_t init_pattern, uint8_t init_grid,
                uint8_t init_i2c_op_address);
void flash_write(uint8_t preset_no, scene_state_t* scene,
                 char (*text)[SCENE_TEXT_LINES][SCENE_TEXT_CHARS]);
uint8_t flash_last_saved_scene(void);
void flash_update_last_saved_scene(uint8_t preset_no);
const char* flash_scene_text(uint8_t preset_no, size_t line);
tele_mode_t flash_last_mode(void);
void flash_update_last_mode(tele_mode_t mode);
void flash_update_cal(cal_data_t*);
void flash_get_cal(cal_data_t*);
void flash_update_cal(cal_data_t*);
void flash_get_cal(cal_data_t*);
void flash_update_device_config(device_config_t*);
void flash_get_device_config(device_config_t*);
void flash_get_scale_bank(uint8_t (*bank)[8]);
void flash_update_scale_bank(uint8_t (*bank)[8]);

// Global per-output CV tuning bank. get copies flash -> RAM, update RAM ->
// flash (called only on an explicit save from the grid tuning view).
void flash_get_tuning(uint16_t (*table)[TUNING_SLOTS]);
void flash_update_tuning(uint16_t (*table)[TUNING_SLOTS]);

// Global Kria song bank (single instance in nvram_data_t; not per-scene).
void flash_get_kria(kria_config_t* dst);
void flash_update_kria(const kria_config_t* src);

// Global i2c follower bank (shared by Kria + MP).
void flash_get_kria_i2c(kria_i2c_fstate_t* dst);
void flash_update_kria_i2c(const kria_i2c_fstate_t* src);

// Global Earthsea bank (single instance in nvram_data_t; not per-scene).
void flash_get_es(es_config_t* dst);
void flash_update_es(const es_config_t* src);

// Global Meadowphysics 8-slot preset bank (not per-scene). Each slot carries a
// config + an 8-byte glyph; save/load a single slot at a time.
void flash_get_mp_slot(uint8_t slot, mp_config_t* cfg, uint8_t glyph[8]);
void flash_update_mp_slot(uint8_t slot, const mp_config_t* cfg,
                          const uint8_t glyph[8]);
uint8_t flash_get_mp_current(void);
void flash_update_mp_current(uint8_t slot);

#endif
