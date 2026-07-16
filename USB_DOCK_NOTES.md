# USB dock / hub notes

Behaviour, fixes, and one open investigation for running a grid + HID + MIDI
through a USB hub or USB-C dock on the Teletype (AVR32 UC3B0512, full-speed-only
USB host, 7 pipes total). Background on the hub port itself lives in
`libavr32/` (`USB_HUB_PORT_PLAN.md`, `HUB_RETRO.md`, `TELETYPE_HUB_PATCH.md`).

## What works

- **Single-tier USB-2 hubs**: grid + HID, or grid + HID + MIDI, enumerate.
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
   when the dock/hub on the **root port** is removed, its downstream devices are
   torn down too, instead of being left orphaned in the device list (dangling
   `->hub`, still holding USB addresses/pipes). Without this, switching from one
   dock to another required a power cycle.

   The sweep is guarded to the **root device only**. A downstream device that
   merely bounces mid-enumeration also disconnects through the same path
   (`uhc_hub_port_change -> uhc_connection_tree(false, d)`, `d != root`); running
   the recursive sweep there frees a device that is still enumerating and
   corrupts it — that broke 3-device enumeration on the Overhub. The root device
   only disconnects when the whole dock is physically removed (an idle moment),
   so sweeping its children there is safe.

2. **USB-disk media gate** (`module/main.c`, `handler_MscConnect`): an empty
   card reader (common in USB-C docks) still enumerates as USB mass storage, and
   enumeration discards the read-capacity result — so a bare MSC connect does not
   mean a drive is present. The module now runs `uhi_msc_mem_test_unit_ready()`
   per LUN and only opens the USB-disk dialog when a LUN reports ready media.
   Previously an empty reader hijacked the UI and blocked grid/HID until it was
   physically unplugged.

## Plug-order constraint (grid + MIDI through a hub)

With a monome **grid + a composite MIDI device (e.g. Elektron Analog Rytm)** on
one hub, **enumeration order matters**:

- **Works:** grid on the **highest**-numbered hub port; MIDI/Rytm on a lower
  port. (The hub is serviced lowest-port-first, so the Rytm enumerates first and
  the grid last.)
- **Fails:** grid on a lower port than the Rytm (grid enumerates first).

**Rule of thumb: put the grid on the highest hub port; MIDI/other devices on
lower ports.** This is a one-time cabling choice and is reliable.

## Open investigation (B) — grid-first -> Rytm enumeration failure

Not yet root-caused. Documented here so it can be picked up later.

**Symptom:** grid + Rytm (+ HID) on one hub. Enumerating **grid first, Rytm
second** fails; **Rytm first, grid second** works. Pre-existing (predates the
fixes above).

**Ruled out by `ioreg` on the actual hardware** (Overhub with all three devices):

- *Not* an EP0 / control-pipe resize + DPRAM-shift issue: every device reports
  `bMaxPacketSize0 = 64`, so the shared control pipe 0 never resizes.
- *Not* pipe-count exhaustion: the grid is a **CDC** device (monome VID 0xCAFE,
  class 2) whose host driver claims only the CDC **data** interface = 2 bulk
  pipes (not 3 — the notification interrupt is not allocated,
  `libavr32/src/usb/cdc/uhi_cdc.c`). Tally: hub 1 + grid 2 + Rytm MIDI 2 + HID 1
  = **6 of 7** pipes.
- *Not* a stray vendor-interface grab: the Rytm is a composite device (class
  239) with an Overbridge vendor interface (class 255, 2 endpoints), but
  `uhi_ftdi` matches FTDI by VID/PID only (`libavr32/src/usb/ftdi/uhi_ftdi.c`),
  so it does not claim it. Only the Rytm's MIDI interface (2 bulk) is allocated.

**Remaining hypothesis:** the one real asymmetry is that the **Rytm is a heavy
composite device** (5 interfaces — 3 vendor + audio-control + MIDI — with IADs
and a large config descriptor), whereas the grid is a simple 2-interface CDC
device. The failure is therefore likely in the **enumeration path for that
composite device through the shared-pipe-0 multi-device control stack**,
sensitive to what enumerated immediately before it — not in pipe or memory
budgeting.

**How to investigate:** re-add the on-module enumeration logging used earlier
(per-step transfer status to the OLED: `usb_enum` status + `uhc_enumeration_stepN`
`uhd_trans_status_t` for step6/step10/step12/step13; see git history of
`libavr32/src/usb.c` and `uhc.c` for the `USB_TOPO_DEBUG` instrumentation).
Reproduce grid-first -> Rytm and read exactly which control transfer fails and
its status code, then trace the shared-pipe-0 handling
(`libavr32/asf/avr32/drivers/usbb/usbb_host.c`) for that transfer.
