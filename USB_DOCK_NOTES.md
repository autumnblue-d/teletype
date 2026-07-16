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

## Grid + MIDI through a hub: root cause & fix (was: plug-order constraint)

**Resolved.** Grid + composite MIDI device (e.g. Elektron Analog Rytm) on one
hub now work in any port order, and survive hot-plugging the MIDI device
while the grid runs. The failure was never a Rytm *enumeration* problem — the
Rytm enumerated fine; the victim was the **grid**, dark whenever the Rytm was
present in the wrong pipe order.

Root cause, in layers (found with the `USB_TOPO_DEBUG` trace, below):

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

## USB debug facility (USB_TOPO_DEBUG)

```bash
cd module && make clean && make USB_TOPO_DEBUG=1
```

builds a firmware with a USB trace ring on the OLED (`libavr32/src/usb_dbg.c`;
normal builds are unaffected — everything is compile-gated). **ALT+F10**
toggles between the trace overlay and the normal UI. **ALT+F9** dumps the
live USBB pipe table. Newest trace line at the bottom.

Trace legend:

- `P+ <port>` / `P- <port>` — hub reported connect/disconnect on that port
- `s14 t<S> n<L>` / `s15 t<S> n<L>` — enumeration transfer completions (full
  config-descriptor read / SET_CONFIGURATION) with status S and length L.
  `n` on s14 identifies the device by descriptor size: grid ≈ 75, Analog
  Rytm ≈ 414, HID dongle ≈ 59, hub 25.
  Status (`uhd_trans_status_t`): 0 ok, 1 disconnect, 2 CRC, 3 data-toggle,
  4 STALL, 5 not responding, 6 PID failure, 7 timeout, 8 aborted.
- `ERR <status> <try>` — enumeration attempt failed
  (`uhc_enum_status_t`: 1 unsupported, 3 fail, 4 hardware limit) on retry
  `<try>` (gives up after 4); `E <addr> <status>` — enumeration finished
  (0 = success)
- `rstTO p<port>` — hub port reset timed out
- `sc t <status>` / `sc ok <n>` — hub status-poll error streak / recovery;
  `poll!` — the status poll could not be re-armed
- `cdcC a<addr>` / `cdcB a<addr>` / `cdcU a<addr>` — monome CDC driver
  claimed / turned away / released that device; `cdc m` + `chg+` — CDC
  enable matched its device, serial-connect event posted
- `mset` / `mrx <v>` / `mcon` — grid setup dialogue started / size reply
  (3 = proper SIZE answer) / monome connect event reached the module
- `gW <n>` — grid write attempts (1st + every 64th); `TXok <n>` /
  `TXE <status> <n>` — grid bulk-OUT clean/error completions (1st + every
  64th); `RXE <status>` — grid bulk-IN error streaks (status 7 = the idle
  20 ms poll timeout, benign)
- `evQ!` — event queue overflowed; an event was silently dropped
- `STORM <UHINT> <UHINTE>` (hex, top line) — USB IRQ-storm detector, drawn
  directly from interrupt context so it works even when the main loop is
  dead. Bit 5 = SOF, bit 8+p = pipe p. Tripping within ~a second of a freeze
  = interrupt storm; tripping only after ~100 s with just the SOF bit = the
  main loop died while USB stayed healthy.
- ALT+F9 pipe dump: `p<N> a<addr> e<ep> [E][F][C]` — per-pipe target
  address, endpoint address, Enabled / Frozen / Config-OK.

## Reporting a hub compatibility issue

Screen a candidate hub/dock on a computer first: plug it in and check the
topology (`ioreg -p IOUSB -w 0` on macOS, `lsusb -t` on Linux). One hub
device (or a USB3/USB2 pair at the same level) is compatible; a hub nested
under another hub means only the outer tier will be serviced. Then reproduce
with a `USB_TOPO_DEBUG` build and include the visible trace lines and the
ALT+F9 pipe table in the report.
