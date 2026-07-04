#ifndef _FLASH_H_
#define _FLASH_H_

#include <stdint.h>

#include "globals.h"
#include "kria_engine.h"
#include "line_editor.h"
#include "teletype.h"

// Reduced from 32 to 30 to reclaim ~12 KB of NVRAM: this lets the flash NVRAM
// region shrink (see __flash_nvram_size__ in config.mk), freeing program flash
// for the Meadowphysics mode, while still leaving room for MP's per-scene data
// (Phase 7). See MEADOWPHYSICS_PORT_PLAN.md.
// Kria (Scenario B): reduced 30 -> 20. Dropping 10 scenes (~63 KB) funds the
// 16-pattern global Kria bank (~18.5 KB) and leaves ~49 KB of program flash for
// Kria code. See KRIA_PORT_PLAN.md §0.
#define SCENE_SLOTS 20
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
    mp_config_t mp;  // Meadowphysics per-scene config
} nvram_scene_t;

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

#endif
