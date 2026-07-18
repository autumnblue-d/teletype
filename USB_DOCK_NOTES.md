# USB dock / hub notes

Behaviour and fixes for running a grid + HID + MIDI through a USB hub or
USB-C dock on the Teletype (AVR32 UC3B0512, full-speed-only USB host, 7
pipes total). Background on the hub port itself lives in `libavr32/`
(`USB_HUB_PORT_PLAN.md`, `HUB_RETRO.md`, `TELETYPE_HUB_PATCH.md`).

## What works

- **Single-tier USB-2 hubs**: grid + HID, or grid + HID + MIDI, enumerate
  and run — in **any port order** (the old "grid on the highest port" rule
  is obsolete, see below), and survive hot-plugging the MIDI device.
- **USB-C docks** (e.g. Genesys-Logic-based): the USB-2 side is used; the
  SuperSpeed side is dead (the UC3B host is USB-2 full-speed only).
- **Cascaded (hub-behind-hub) docks** such as the Elektron Overhub: only the
  **first** hub tier is serviced (`UHI_HUB_MAX = 1` in
  `libavr32/src/usb/hub/uhi_hub.h`). Devices on tier-1 ports work; devices on
  the inner tier do not. Bumping `UHI_HUB_MAX` to 2 enables the inner tier but
  costs a second hub-status pipe, dropping the device budget below what
  grid+HID+MIDI needs — so it is intentionally left at 1.

## Fixes in this change set

1. **Hub-unplug teardown** (`libavr32/.../uhc/uhc.c`, `uhc_connection_tree`):
   when the dock/hub on the **root port** is removed, its downstream devices
   are torn down too, instead of being left orphaned in the device list.
   Without this, switching from one dock to another required a power cycle.
   The sweep is guarded to the root device only: a downstream device that
   bounces mid-enumeration also disconnects through the same path, and
   sweeping there frees a device that is still enumerating.

2. **Duplicate-connect guard** (`uhc.c`, `uhc_hub_port_change`): a connector
   bounce can latch one connection change while port status already shows
   connected again. The stale device on that (hub, port) is torn down first,
   then the new arrival enumerates — previously the old `uhc_device_t` was
   orphaned still holding its pipes and USB address.

3. **USB-disk media gate** (`module/main.c`, `handler_MscConnect`): an empty
   card reader (common in USB-C docks) still enumerates as USB mass storage.
   The module now runs `uhi_msc_mem_test_unit_ready()` per LUN and only opens
   the USB-disk dialog when a LUN reports ready media. Note: the check runs
   once at connect — a card inserted *later* is not detected until the reader
   is replugged.

4. **Bulk NAK throttle** and **enumeration/grid serialization** — see below.

5. **Hub-class hardening** (`libavr32/src/usb/hub/uhi_hub.c`): hub-level
   status changes are serviced (an over-current trip is acked and ports are
   automatically re-powered); up to 7 ports are powered and watched (was 4;
   all reported ports get PORT_POWER even beyond 7); the `bPwrOn2PwrGood`
   settle time is honoured before the first status poll; GetHubDescriptor
   sends the descriptor type in wValue (some hubs STALL a zero wValue); the
   config-descriptor walk guards against malformed descriptors; and every
   transfer-error / submit-failure path now retries via a 1 ms SOF engine
   (`uhi_hub_sof`) instead of silently dead-stalling hub servicing.

## Grid + MIDI through a hub: root cause & fix (was: plug-order constraint)

**Resolved.** Grid + composite MIDI device (e.g. Elektron Analog Rytm) on one
hub now work in any port order, and survive hot-plugging the MIDI device
while the grid runs. The failure was never a Rytm *enumeration* problem — the
Rytm enumerated fine; the victim was the **grid**, dark whenever the Rytm was
present in the wrong pipe order.

Root cause, in layers:

1. **The USBB host retries a NAKed bulk pipe continuously**: a NAK does not
   decrement UPINRQ and bulk pipes have no interval, so an idle bulk-IN poll
   (the MIDI driver's read, armed essentially 100% of the time) monopolizes
   the bus. Which device starved depended on pipe allocation order — hence
   the port-order dependence. Enumeration behaves the same way (slow control
   responses NAK-spam the shared pipe 0), which is why the grid died before
   the Rytm's enumeration even completed.
2. **The grid shows starvation as total darkness**: its LED writes ride a
   20 ms transfer timeout (`UHI_MCDC_TIMEOUT`) and `cdc_write()` silently
   drops frames while one is pending. MIDI, with a 20 s timeout and a TX
   ring, shrugged the same contention off — so it always "worked".
3. **The grid's setup dialogue ran concurrently with the next device's
   enumeration**, exposing the connect-time window to the same contention.

Fixes:

- `libavr32/asf/avr32/drivers/usbb/usbb_host.c` — **bulk NAK throttle**: on
  the first NAK of a bulk transfer the pipe is frozen and retried at the next
  SOF (one token per ms per idle pipe instead of a continuous hammer). This
  is the root-cause fix.
- `module/main.c` + `libavr32/src/usb.{c,h}` — grid traffic serialized
  against enumeration via `usb_enumeration_active`: the monome poll skips
  while any device is enumerating, and the monome setup dialogue is deferred
  until enumeration has been quiet for 50 ms (`MONOME_SETUP_QUIET_TICKS`).

## USB disk mode (scene backup) — two latent bugs fixed

Testing scene WRITE/READ through the new MSC media gate surfaced two dormant
bugs (July 2026):

1. **Stale FAT sector cache**: the cache descriptor is a zero-initialized
   global whose "empty" marker is 0xFF, so untouched it claims "LUN 0,
   sector 0 already loaded" and the first mount reads 512 stale zero bytes
   instead of the MBR — `FS_ERR_NO_FORMAT` on a perfectly good FAT32/MBR
   stick. Stock firmware escaped by accident (the first TUR on a fresh drive
   returned BUSY, whose retry path resets the cache); the media gate walks
   the LUN to GOOD first and removed the accident. Fix: `nav_reset()` at
   `tele_usb_disk()` entry.
2. **Disk-mode stack overflow (this branch only)**: the write/read
   operations stack-allocated a `scene_state_t`, which the Kria/MP/ES work
   grew to ~18.7 KB — an ~11 KB dive past the 8 KB stack. Symptoms: firmware
   memory sprayed into scene files (0x8005xxxx pointer tables), hangs after
   writing, no files at all, varying run to run. Fix: both operations stage
   through the global `scene_state`/`scene_text` and the live scene is
   restored from flash on exit. **Trade-off: unsaved live-scene edits are
   lost across a USB disk operation** (no RAM exists for a private copy).

Also hardened: the LUN-count query at disk entry is bounded (3 s) so a stick
that vanishes mid-operation aborts cleanly instead of hanging the module.

## Raw NVRAM image backup / restore (`RAW BACKUP` / `RAW RESTORE`)

Two extra USB-disk menu items save and reload the **entire** `nvram_data_t` as
one binary blob, `ttnvram.bin`. Unlike the per-scene `tt##.txt` path, the raw
image also captures every global bank the text serializer never touches — cal,
device_config, and the kria / earthsea / mp / tuning / scale banks. It is a
same-firmware *snapshot/clone* tool, not a portable or editable backup: the
image is a byte-for-byte copy of flash, welded to this exact layout.

Mechanics (`module/flash.c` accessors + `module/usb_disk_mode.c`):

- `flash_nvram_image()` returns `&f` (memory-mapped flash), so BACKUP streams it
  straight into the file; RESTORE streams file → `flash_nvram_write_chunk()`
  (`flashc_memcpy`). Both go in 512 B chunks — the ~127 KB image fits in neither
  the 8 KB stack nor 64 KB SRAM.
- RESTORE is gated **before any flash is erased**: the file length must equal
  `flash_nvram_size()`, and the image's `FIRSTRUN_KEY` tag (read at
  `flash_nvram_fresh_offset()`) must satisfy `flash_nvram_image_compatible()`.
  Wrong length → `NO IMAGE`; wrong tag → `BAD VERSION`. This is what stops a
  foreign-layout image from corrupting NVRAM. A compatible image already carries
  the correct tag, so there is no separate commit step.
- Like the per-scene restore, the write itself is **not power-atomic** — a
  power loss mid-restore leaves a partial image (`FAILED`). Global banks in RAM
  only refresh on the next boot, hence the `OK - REBOOT` result message; scenes
  reload immediately via `handler_usb_Front`.

The PARAM-knob menu mapping was generalised from 4 to `USB_MENU_ITEM_COUNT`
items (now 6, filling OLED lines 2..7; lines 0..1 carry operation status).

**Flash budget:** this feature consumed essentially all remaining program flash
on this branch — `.data` LMA end now sits exactly at the `.flash_nvram` base
(`0x80060000`), i.e. ~0 bytes free. It did **not** change the NVRAM layout, so
`FIRSTRUN_KEY` was not bumped and existing scenes survive the upgrade. To buy
headroom back, lower `__flash_nvram_size__` (config.mk) to the page boundary
above `sizeof(nvram_data_t)` (0x1FC98 → 0x1FE00 frees 512 B) and bump
`FIRSTRUN_KEY` — but that forces a one-time reseed.


## Reporting a hub compatibility issue

Screen a candidate hub/dock on a computer first: plug it in and check the
topology (`ioreg -p IOUSB -w 0` on macOS, `lsusb -t` on Linux). One hub
device (or a USB3/USB2 pair at the same level) is compatible; a hub nested
under another hub means only the outer tier will be serviced. Then reproduce
on the module and describe the hub/dock, the attached devices, and which ones
work versus fail.
