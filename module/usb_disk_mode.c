#include "usb_disk_mode.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>


#include "flash.h"
#include "globals.h"
#include "scene_serialization.h"

// libavr32
#include "adc.h"
#include "font.h"
#include "interrupts.h"
#include "region.h"
#include "util.h"

// asf
#include "delay.h"
#include "fat.h"
#include "file.h"
#include "fs_com.h"
#include "navigation.h"
#include "print_funcs.h"
#include "uhd.h"
#include "uhi_msc.h"
#include "uhi_msc_mem.h"
#include "usb_protocol_msc.h"


// Local declarations
void draw_usb_menu_item(uint8_t item_num, const char* text);
bool tele_usb_disk_write_operation(uint8_t* plun_state, uint8_t* plun);
void tele_usb_disk_read_operation(void);
void tele_usb_disk_raw_write_operation(void);
void tele_usb_disk_raw_read_operation(void);


// Local functions to implement the usb filesystem serialization contract
void tele_usb_putc(void* self_data, uint8_t c);
void tele_usb_write_buf(void* self_data, uint8_t* buffer, uint16_t size);
uint16_t tele_usb_getc(void* self_data);
bool tele_usb_eof(void* self_data);


void tele_usb_putc(void* self_data, uint8_t c) {
    file_putc(c);
}

void tele_usb_write_buf(void* self_data, uint8_t* buffer, uint16_t size) {
    file_write_buf(buffer, size);
}

uint16_t tele_usb_getc(void* self_data) {
    return file_getc();
}

bool tele_usb_eof(void* self_data) {
    return file_eof() != 0;
}

// *very* basic USB operations menu


typedef enum {
    USB_MENU_COMMAND_WRITE = 0,
    USB_MENU_COMMAND_READ = 1,
    USB_MENU_COMMAND_BOTH = 2,
    USB_MENU_COMMAND_RAW_WRITE = 3,
    USB_MENU_COMMAND_RAW_READ = 4,
    USB_MENU_COMMAND_EXIT = 5,
} usb_menu_command_t;

#define USB_MENU_ITEM_COUNT (USB_MENU_COMMAND_EXIT + 1)

usb_menu_command_t usb_menu_command;

void draw_usb_menu_item(uint8_t item_num, const char* text) {
    // Six items now fill lines 2..7, leaving lines 0..1 for operation status.
    uint8_t line_num = 2 + item_num;
    uint8_t fg = usb_menu_command == item_num ? 0 : 0xa;
    uint8_t bg = usb_menu_command == item_num ? 0xa : 0;
    region_fill(&line[line_num], bg);
    font_string_region_clip_tab(&line[line_num], text, 2, 0, fg, bg);
    region_draw(&line[line_num]);
}

void handler_usb_PollADC(int32_t data) {
    uint16_t adc[4];
    adc_convert(&adc);
    // Map the 12-bit PARAM knob across USB_MENU_ITEM_COUNT items. Odd raw
    // values are a deadzone between adjacent items so a knob resting near a
    // boundary doesn't flicker (matches the original >>9 scheme, generalised
    // from 4 items to the current count).
    uint32_t raw = ((uint32_t)adc[1] * (USB_MENU_ITEM_COUNT * 2)) >> 12;
    if (raw > USB_MENU_ITEM_COUNT * 2 - 1) raw = USB_MENU_ITEM_COUNT * 2 - 1;
    uint8_t deadzone = raw & 1;
    uint8_t cursor = raw >> 1;
    if (!deadzone || abs(cursor - usb_menu_command) > 1) {
        usb_menu_command = cursor;
    }
}

void handler_usb_Front(int32_t data) {
    // disable timers
    u8 flags = irqs_pause();

    if (usb_menu_command != USB_MENU_COMMAND_EXIT) {
        tele_usb_disk();
        // The disk operations staged every slot through the global
        // scene_state/scene_text (an 18.7 KB scene_state_t cannot live on
        // the 8 KB stack); bring the live scene back from flash. Unsaved
        // live edits are lost across a USB disk operation.
        ss_set_scene(&scene_state, preset_select);
        flash_read(preset_select, &scene_state, &scene_text, 1, 1, 1);
    }

    // renable teletype
    set_mode(M_LIVE);
    assign_main_event_handlers();
    irqs_resume(flags);
}

void handler_usb_ScreenRefresh(int32_t data) {
    draw_usb_menu_item(0, "WRITE TO USB");
    draw_usb_menu_item(1, "READ FROM USB");
    draw_usb_menu_item(2, "DO BOTH");
    draw_usb_menu_item(3, "RAW BACKUP");
    draw_usb_menu_item(4, "RAW RESTORE");
    draw_usb_menu_item(5, "EXIT");
}


// usb disk mode entry point
void tele_usb_disk() {
    print_dbg("\r\nusb");
    uint8_t lun_state = 0;

    // Reset the FAT navigation state, INCLUDING the sector cache. The cache
    // descriptor is a zero-initialized global whose empty marker is 0xFF, so
    // untouched it claims "LUN 0, sector 0 already loaded" and mount reads
    // 512 zero bytes instead of the MBR (FS_ERR_NO_FORMAT on a good stick).
    // Vanilla survived by accident: fat_check_device()'s first TUR on a fresh
    // drive returned BUSY, whose retry path calls fat_cache_reset(). The MSC
    // media gate in handler_MscConnect now walks the LUN to GOOD before the
    // FAT layer ever runs, so that accidental reset no longer happens.
    nav_reset();

    // uhi_msc_mem_* calls spin forever on uhi_msc_is_available() if the stick
    // vanished (bounce / unplug / re-enumeration). Bound the wait once here
    // and bail out cleanly instead of hanging the module inside the handler.
    uint16_t avail_wait = 0;
    while (!uhi_msc_is_available()) {
        if (++avail_wait > 3000) {
            return;
        }
        delay_ms(1);
    }

    // The USBB bulk NAK throttle freezes a NAKed bulk pipe until the next SOF,
    // which serializes MSC scene read/write to one token per 1 ms frame. Disk
    // mode is exclusive -- the grid/MIDI handlers are swapped out
    // (assign_msc_event_handlers), so nothing else is on the bus here and the
    // throttle only slows us down. Run full-speed for the mount/read/write
    // below, then restore it so the grid/MIDI starvation cure is back for
    // normal operation. Scoped after the early "d:gone" return above so that
    // bail-out path leaves the throttle in its default (enabled) state.
    uhd_bulk_nak_throttle_set(false);

    for (uint8_t lun = 0; (lun < uhi_msc_mem_get_lun()) && (lun < 8); lun++) {
        // print_dbg("\r\nlun: ");
        // print_dbg_ulong(lun);

        // Mount drive
        nav_drive_set(lun);
        if (!nav_partition_mount()) {
            if (fs_g_status == FS_ERR_HW_NO_PRESENT) {
                // The test can not be done, if LUN is not present
                lun_state &= ~(1 << lun);  // LUN test reseted
                continue;
            }
            lun_state |= (1 << lun);  // LUN test is done.
            print_dbg("\r\nfail");
            // ui_test_finish(false); // Test fail
            continue;
        }
        // Check if LUN has been already tested
        if (lun_state & (1 << lun)) { continue; }

        if (usb_menu_command == USB_MENU_COMMAND_WRITE ||
            usb_menu_command == USB_MENU_COMMAND_BOTH) {
            if (!tele_usb_disk_write_operation(&lun_state, &lun)) { continue; }
        }
        if (usb_menu_command == USB_MENU_COMMAND_READ ||
            usb_menu_command == USB_MENU_COMMAND_BOTH) {
            tele_usb_disk_read_operation();
        }
        if (usb_menu_command == USB_MENU_COMMAND_RAW_WRITE) {
            tele_usb_disk_raw_write_operation();
        }
        if (usb_menu_command == USB_MENU_COMMAND_RAW_READ) {
            tele_usb_disk_raw_read_operation();
        }

        nav_exit();
    }

    // restore the bulk NAK throttle disabled before the loop
    uhd_bulk_nak_throttle_set(true);

}

bool tele_usb_disk_write_operation(uint8_t* plun_state, uint8_t* plun) {
    // WRITE SCENES
    print_dbg("\r\nwriting scenes");

    char filename[13];
    strcpy(filename, "tt00s.txt");

    char text_buffer[40];
    strcpy(text_buffer, "WRITE");
    region_fill(&line[0], 0);
    font_string_region_clip_tab(&line[0], text_buffer, 2, 0, 0xa, 0);
    region_draw(&line[0]);

    for (int i = 0; i < SCENE_SLOTS; i++) {
        // Stage through the GLOBAL scene_state/scene_text: scene_state_t is
        // ~18.7 KB on this branch (Kria/MP/ES state), so a stack copy here
        // overflowed the 8 KB stack and sprayed firmware memory into the
        // written files. The live scene is restored from flash when disk
        // mode exits (handler_usb_Front).
        ss_init(&scene_state);
        memset(scene_text, 0, SCENE_TEXT_LINES * SCENE_TEXT_CHARS);

        strcat(text_buffer, ".");  // strcat is dangerous, make sure the
                                   // buffer is large enough!
        region_fill(&line[0], 0);
        font_string_region_clip_tab(&line[0], text_buffer, 2, 0, 0xa, 0);
        region_draw(&line[0]);

        flash_read(i, &scene_state, &scene_text, 1, 1, 1);

        if (!nav_file_create((FS_STRING)filename)) {
            if (fs_g_status != FS_ERR_FILE_EXIST) {
                if (fs_g_status == FS_LUN_WP) {
                    // Test can be done only on no write protected
                    // device
                    return false;
                }
                *plun_state |= (1 << *plun);  // LUN test is done.
                print_dbg("\r\nfail");
                return false;
            }
        }

        if (!file_open(FOPEN_MODE_W)) {
            if (fs_g_status == FS_LUN_WP) {
                // Test can be done only on no write protected
                // device
                return false;
            }
            *plun_state |= (1 << *plun);  // LUN test is done.
            print_dbg("\r\nfail");
            return false;
        }

        tt_serializer_t tele_usb_writer;
        tele_usb_writer.write_char = &tele_usb_putc;
        tele_usb_writer.write_buffer = &tele_usb_write_buf;
        tele_usb_writer.print_dbg = &print_dbg;
        tele_usb_writer.data =
            NULL;  // asf disk i/o holds state, no handles needed
        serialize_scene(&tele_usb_writer, &scene_state, &scene_text);

        file_close();
        *plun_state |= (1 << *plun);  // LUN test is done.

        if (filename[3] == '9') {
            filename[3] = '0';
            filename[2]++;
        }
        else
            filename[3]++;

        print_dbg(".");
    }

    nav_filelist_reset();
    return true;
}

void tele_usb_disk_read_operation() {
    // READ SCENES
    print_dbg("\r\nreading scenes...");

    char filename[13];
    strcpy(filename, "tt00.txt");

    char text_buffer[40];
    strcpy(text_buffer, "READ");
    region_fill(&line[1], 0);
    font_string_region_clip_tab(&line[1], text_buffer, 2, 0, 0xa, 0);
    region_draw(&line[1]);

    for (int i = 0; i < SCENE_SLOTS; i++) {
        // Global staging: see the matching comment in the write operation —
        // a stack scene_state_t overflows the 8 KB stack on this branch.
        ss_init(&scene_state);
        memset(scene_text, 0, SCENE_TEXT_LINES * SCENE_TEXT_CHARS);

        strcat(text_buffer, ".");  // strcat is dangerous, make sure the
                                   // buffer is large enough!
        region_fill(&line[1], 0);
        font_string_region_clip_tab(&line[1], text_buffer, 2, 0, 0xa, 0);
        region_draw(&line[1]);
        if (nav_filelist_findname(filename, 0)) {
            print_dbg("\r\nfound: ");
            print_dbg(filename);
            if (!file_open(FOPEN_MODE_R))
                print_dbg("\r\ncan't open");
            else {
                tt_deserializer_t tele_usb_reader;
                tele_usb_reader.read_char = &tele_usb_getc;
                tele_usb_reader.eof = &tele_usb_eof;
                tele_usb_reader.print_dbg = &print_dbg;
                tele_usb_reader.data =
                    NULL;  // asf disk i/o holds state, no handles needed
                deserialize_scene(&tele_usb_reader, &scene_state, &scene_text);

                file_close();
                flash_write(i, &scene_state, &scene_text);
            }
        }

        nav_filelist_reset();

        if (filename[3] == '9') {
            filename[3] = '0';
            filename[2]++;
        }
        else
            filename[3]++;
    }
}

// --- Raw NVRAM image backup / restore -----------------------------------
//
// One binary file holding the entire nvram_data_t exactly as it sits in flash:
// every scene PLUS every global bank (cal, device_config, kria/es/mp/tuning),
// which the per-scene tt##.txt path does not capture. The image is welded to
// this firmware's layout (see flash.c), so a restore is length- and tag-gated
// before any flash is erased. Streamed in 512 B chunks — the ~127 KB image
// can't be buffered in the 8 KB stack or the 64 KB SRAM.

// Distinct from the per-scene tt##.txt files (FAT 8.3).
#define RAW_IMAGE_FILENAME "ttnvram.bin"
#define RAW_CHUNK 512

static void raw_status(uint8_t line_num, const char* text) {
    region_fill(&line[line_num], 0);
    font_string_region_clip_tab(&line[line_num], (char*)text, 2, 0, 0xa, 0);
    region_draw(&line[line_num]);
}

void tele_usb_disk_raw_write_operation() {
    char filename[13];
    strcpy(filename, RAW_IMAGE_FILENAME);

    if (!nav_file_create((FS_STRING)filename) &&
        fs_g_status != FS_ERR_FILE_EXIST) {
        raw_status(1, "FAILED");
        return;
    }
    if (!file_open(FOPEN_MODE_W)) {
        raw_status(1, "FAILED");
        return;
    }

    const uint8_t* img = (const uint8_t*)flash_nvram_image();
    uint32_t size = flash_nvram_size();
    for (uint32_t off = 0; off < size;) {
        uint16_t want =
            (size - off) > RAW_CHUNK ? RAW_CHUNK : (uint16_t)(size - off);
        // img is memory-mapped flash; read it straight into the file buffer.
        file_write_buf((uint8_t*)(img + off), want);
        off += want;
    }
    file_set_eof(); // truncate to exactly the image size
    file_close();
    nav_filelist_reset();
    raw_status(1, "DONE");
}

void tele_usb_disk_raw_read_operation() {
    char filename[13];
    strcpy(filename, RAW_IMAGE_FILENAME);

    uint32_t size = flash_nvram_size();
    if (!nav_filelist_findname((FS_STRING)filename, 0) ||
        nav_file_lgt() != size || !file_open(FOPEN_MODE_R)) {
        // Missing, wrong length (not our image / different layout), or unreadable
        raw_status(1, "NO IMAGE");
        return;
    }

    // Compatibility gate: read the image's validity tag and refuse a foreign
    // image BEFORE erasing any flash, so a mismatched build can't corrupt the
    // running NVRAM. A matching image already carries the correct tag.
    file_seek(flash_nvram_fresh_offset(), FS_SEEK_SET);
    if (!flash_nvram_image_compatible((uint8_t)file_getc())) {
        file_close();
        raw_status(1, "BAD VERSION");
        return;
    }
    file_seek(0, FS_SEEK_SET);

    uint8_t buf[RAW_CHUNK];
    uint32_t off = 0;
    while (off < size) {
        uint16_t want =
            (size - off) > RAW_CHUNK ? RAW_CHUNK : (uint16_t)(size - off);
        uint16_t got = file_read_buf(buf, want);
        if (got == 0) break; // short read: partial restore (not power-atomic)
        flash_nvram_write_chunk(off, buf, got);
        off += got;
    }
    file_close();
    nav_filelist_reset();
    // Scenes reload from the restored flash when disk mode exits
    // (handler_usb_Front); the global banks in RAM refresh on the next boot.
    raw_status(1, off == size ? "OK - REBOOT" : "FAILED");
}
