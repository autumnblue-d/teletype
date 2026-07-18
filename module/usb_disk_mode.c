#include "usb_disk_mode.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#ifdef USB_TOPO_DEBUG
#include "usb_dbg.h" // disk-mode failure probes; read via ALT+F10 after exit
#endif

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
#include "uhi_msc.h"
#include "uhi_msc_mem.h"
#include "usb_protocol_msc.h"

#ifdef USB_TOPO_DEBUG
// Stage marker drawn IMMEDIATELY on the bottom OLED line: if the disk code
// blocks forever (the FAT/MSC layers busy-wait on USB completions), the last
// marker on screen names the exact call it never returned from.
// Also calms the IRQ-storm detector: disk operations run for minutes inside
// the front-button handler where the normal calm site (ScreenRefresh) never
// runs, and a false STORM trip draws over the disk UI from interrupt context.
static void dsk_mark(const char* s) {
    usb_dbg_irq_calm();
    region_fill(&line[7], 0);
    font_string_region_clip_tab(&line[7], (char*)s, 2, 0, 0xa, 0);
    region_draw(&line[7]);
}
#else
#define dsk_mark(s)
#endif

// Local declarations
void draw_usb_menu_item(uint8_t item_num, const char* text);
bool tele_usb_disk_write_operation(uint8_t* plun_state, uint8_t* plun);
void tele_usb_disk_read_operation(void);


// Local functions to implement the usb filesystem serialization contract
void tele_usb_putc(void* self_data, uint8_t c);
void tele_usb_write_buf(void* self_data, uint8_t* buffer, uint16_t size);
uint16_t tele_usb_getc(void* self_data);
bool tele_usb_eof(void* self_data);

#ifdef USB_TOPO_DEBUG
// First serialize-write failure of a streak: "pcE <fs_g_status>". file_putc
// errors are otherwise swallowed and produce silently truncated files.
static uint8_t usb_dbg_putc_failed;
static void usb_dbg_putc_check(bool ok) {
    if (ok) { usb_dbg_putc_failed = 0; }
    else if (!usb_dbg_putc_failed) {
        usb_dbg_putc_failed = 1;
        usb_dbg_log_val("pcE", fs_g_status);
    }
}
#endif

void tele_usb_putc(void* self_data, uint8_t c) {
#ifdef USB_TOPO_DEBUG
    usb_dbg_putc_check(file_putc(c) != 0);
#else
    file_putc(c);
#endif
}

void tele_usb_write_buf(void* self_data, uint8_t* buffer, uint16_t size) {
#ifdef USB_TOPO_DEBUG
    usb_dbg_putc_check(file_write_buf(buffer, size) == size);
#else
    file_write_buf(buffer, size);
#endif
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
    USB_MENU_COMMAND_EXIT = 3,
} usb_menu_command_t;

usb_menu_command_t usb_menu_command;

void draw_usb_menu_item(uint8_t item_num, const char* text) {
    uint8_t line_num = 4 + item_num;
    uint8_t fg = usb_menu_command == item_num ? 0 : 0xa;
    uint8_t bg = usb_menu_command == item_num ? 0xa : 0;
    region_fill(&line[line_num], bg);
    font_string_region_clip_tab(&line[line_num], text, 2, 0, fg, bg);
    region_draw(&line[line_num]);
}

void handler_usb_PollADC(int32_t data) {
    uint16_t adc[4];
    adc_convert(&adc);
    uint8_t cursor = adc[1] >> 9;
    uint8_t deadzone = cursor & 1;
    cursor >>= 1;
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
#ifdef USB_TOPO_DEBUG
    usb_dbg_irq_calm(); // main loop alive at the disk menu; see dsk_mark
#endif
    draw_usb_menu_item(0, "WRITE TO USB");
    draw_usb_menu_item(1, "READ FROM USB");
    draw_usb_menu_item(2, "DO BOTH");
    draw_usb_menu_item(3, "EXIT");
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

    dsk_mark("d:lun"); // hang here = uhi_msc_mem_get_lun/is_available wait
    // uhi_msc_mem_* calls spin forever on uhi_msc_is_available() if the stick
    // vanished (bounce / unplug / re-enumeration). Bound the wait once here
    // and bail out cleanly instead of hanging the module inside the handler.
    uint16_t avail_wait = 0;
    while (!uhi_msc_is_available()) {
        if (++avail_wait > 3000) {
            dsk_mark("d:gone"); // MSC device left and never came back
            return;
        }
        delay_ms(1);
    }
    for (uint8_t lun = 0; (lun < uhi_msc_mem_get_lun()) && (lun < 8); lun++) {
        // print_dbg("\r\nlun: ");
        // print_dbg_ulong(lun);

        // Mount drive
        nav_drive_set(lun);
        dsk_mark("d:mnt"); // hang here = mount reads never complete
        if (!nav_partition_mount()) {
#ifdef USB_TOPO_DEBUG
            usb_dbg_log2("mnt", lun, fs_g_status);
#endif
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

        dsk_mark("d:mok"); // mount succeeded
        if (usb_menu_command == USB_MENU_COMMAND_WRITE ||
            usb_menu_command == USB_MENU_COMMAND_BOTH) {
            if (!tele_usb_disk_write_operation(&lun_state, &lun)) { continue; }
        }
        if (usb_menu_command == USB_MENU_COMMAND_READ ||
            usb_menu_command == USB_MENU_COMMAND_BOTH) {
            tele_usb_disk_read_operation();
        }

        dsk_mark("d:ex"); // hang here = nav_exit (FAT/dir flush writes)
        nav_exit();
        dsk_mark("d:nx"); // hang here = next-LUN availability wait
    }
    dsk_mark("d:end"); // disk operation complete; returning to the menu exit
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

        dsk_mark("w:cr"); // hang here = file create never completes
        if (!nav_file_create((FS_STRING)filename)) {
#ifdef USB_TOPO_DEBUG
            if (fs_g_status != FS_ERR_FILE_EXIST)
                usb_dbg_log2("crE", i, fs_g_status);
#endif
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

        dsk_mark("w:op");
        if (!file_open(FOPEN_MODE_W)) {
#ifdef USB_TOPO_DEBUG
            usb_dbg_log2("opE", i, fs_g_status);
#endif
            if (fs_g_status == FS_LUN_WP) {
                // Test can be done only on no write protected
                // device
                return false;
            }
            *plun_state |= (1 << *plun);  // LUN test is done.
            print_dbg("\r\nfail");
            return false;
        }

        dsk_mark("w:sz"); // hang here = serialize write never completes
        tt_serializer_t tele_usb_writer;
        tele_usb_writer.write_char = &tele_usb_putc;
        tele_usb_writer.write_buffer = &tele_usb_write_buf;
        tele_usb_writer.print_dbg = &print_dbg;
        tele_usb_writer.data =
            NULL;  // asf disk i/o holds state, no handles needed
        serialize_scene(&tele_usb_writer, &scene_state, &scene_text);

        file_close();
        dsk_mark("w:cl"); // scene file closed
        *plun_state |= (1 << *plun);  // LUN test is done.

        if (filename[3] == '9') {
            filename[3] = '0';
            filename[2]++;
        }
        else
            filename[3]++;

        print_dbg(".");
    }

#ifdef USB_TOPO_DEBUG
    usb_dbg_push("wrOK"); // all scenes serialized and closed without bailing
#endif
    dsk_mark("w:fl"); // hang here = nav_filelist_reset directory ops
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
