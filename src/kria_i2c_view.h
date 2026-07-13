#ifndef _KRIA_I2C_VIEW_H_
#define _KRIA_I2C_VIEW_H_

#include <stdint.h>

// Shared i2c-follower grid view (Ansible ii pages), used by the Kria, MP and
// Earthsea shells. Split out of kria_i2c.c so follower output/persistence and
// this grid UI live in separate files. Operates on the follower bank through
// the kria_i2c.h public accessors/setters; edits mark the bank dirty (queried
// via kria_i2c_take_dirty).
//
// enter() resets the view; render() fills a 16x8 led buffer; key() handles a
// press (edits go through the kria_i2c setters, which mark the bank dirty).
void kria_i2c_view_enter(void);
void kria_i2c_view_render(uint8_t* led, uint8_t vari);
void kria_i2c_view_key(uint8_t x, uint8_t y, uint8_t z);

// Poll for a pending request to open the OLED MIDI editor (returns the follower
// index, or -1). Set when the user config-taps a MIDI follower on the grid.
int8_t kria_i2c_view_take_oled_req(void);

#endif
