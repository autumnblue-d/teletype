#ifndef _KRIA_I2C_OLED_H_
#define _KRIA_I2C_OLED_H_

#include <stdint.h>

// Keyboard-driven OLED editor for the MIDI followers (I2M / MO). Renders a
// scrolling LABEL/value field list to the shared `line[]` regions and edits the
// selected follower's config. Shared by the Kria and MP i2c views. See
// KRIA_I2C_PLAN.md. Grid-configured CV followers keep their grid config pages.

void kria_i2c_oled_enter(uint8_t index);  // open editor for a MIDI follower
void kria_i2c_oled_exit(void);
uint8_t kria_i2c_oled_active(void);  // 1 while the editor is capturing keys
void kria_i2c_oled_render(void);     // draw into line[] (call from screen refresh)

// Handle a keyboard key while active. Returns 1 if the editor consumed it.
uint8_t kria_i2c_oled_key(uint8_t key, uint8_t mod, uint8_t is_held_key);

#endif
