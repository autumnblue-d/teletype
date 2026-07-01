# USB Hub Support — Port Plan

Plan for adding USB hub / multi-device support to teletype so more than one
USB device (e.g. a monome grid **plus** a USB keyboard, MIDI controller, or
serial device) can be connected at once.

## Outcome (implemented + hardware-verified)

**Works:** a monome grid (CDC) + a USB keyboard (HID) + a MIDI controller
(Arturia Keystep) all run simultaneously through a powered hub. Built with
`dewb/monome-build`; hub-disabled build is byte-identical to stock firmware.
Enabled by `#define USB_HOST_HUB_SUPPORT` in `conf_usb_host.h`.

**Real bugs found during on-hardware bring-up (all fixed):**
1. `uhc.h` `uhc_device_t` used its own typedef name for self-referential
   pointers in an untagged struct — never compiled. Tagged the struct.
2. Set-address loop assigned address 1 to every device (`while(x++)` never
   entered) → hub/downstream collision. Rewrote as a real free-address search.
3. `uhd_ctrl_phase_data_out` used `ep_ctrl_size` uninitialized in hub mode →
   corrupt control-OUT transfers.
4. Hub-port **reset handshake was a stub** — never fired the enumeration
   continuation, so downstream enumeration stalled. Implemented SET_FEATURE
   (PORT_RESET) → poll GET_STATUS until C_PORT_RESET → clear → continue.
5. Port status-change handler only cleared *connection* changes → uncleared
   change bits made the hub re-assert its interrupt forever ("stuck"). Now
   drains every change type.
6. **`uhi_hub_install` claimed its single slot before confirming hub class** and
   returned `SOFTWARE_LIMIT` for non-hub devices → the grid was killed once the
   hub held the slot. Moved the slot claim after the class check.
7. **UHC dispatch treated `SOFTWARE_LIMIT` as fatal**: any single-instance
   driver with a full slot (e.g. CDC once the grid is attached) aborted a
   *different-class* device's enumeration (the keyboard). Made `SOFTWARE_LIMIT`
   non-fatal (skip that driver, like UNSUPPORTED).
8. `uhd_ep_free(addr, 0xFF)` disabled the **shared control pipe 0** when a
   device left → DISCONNECT on all later control transfers. Protected pipe 0 in
   hub mode.
9. Missing downstream-device lifecycle: added `uhc_hub_port_change()` (malloc +
   link + `uhc_connection_tree`) and a poll-resume hook from `usb_enum` so a
   second device is detected after the first finishes enumerating.

**Known limitations / not addressed:**
- Disconnect detection while another device is mid-enumeration (the poll is
  paused during enumeration); single-at-a-time plugging is fine.
- Failed/looping enumerations can leak a `uhc_device_t` (address climbs); a
  power-cycle resets it. Full teardown-on-failure not wired.
- 3 simultaneous devices is at the UC3B pipe ceiling; grid+keyboard+MIDI fits,
  more may not.
- The **Korg nanoKONTROL2 does not enumerate even directly** (pre-existing
  device/teletype incompatibility, unrelated to the hub).
- All the above are guarded by `USB_HOST_HUB_SUPPORT`; with it off the firmware
  is unchanged.

## TL;DR

- **TinyUSB does not help.** It has no port for the AVR32 / AT32UC3B0512 (no
  `OPT_MCU`, no host controller driver for the USBB peripheral). Its `hub.c` is
  useful only as a *reference to read*, not code that can link here.
- The existing **Atmel ASF UHC/UHI** stack contains hub *scaffolding* but no
  working hub support. It is **unfinished**, not merely disabled behind a flag.
- This is a real firmware project, not a config change. Scope it to **2–3
  simultaneous devices** because of a hardware pipe limit, not RAM.
- **Key correction (verified in code):** the device classes map to UHI drivers
  as follows — **grid = serial (FTDI/CDC), keyboard = HID, controller = MIDI**.
  The grid is *not* a HID device. Because UHC dispatches `install()` to every
  UHI driver per device (`uhc.c:700`), the **high-value combos (grid + keyboard,
  grid + MIDI, keyboard + MIDI) need only the UHC hub layer — the existing
  single-instance drivers already suffice**, since each is a different class.
  Per-driver multi-instancing is only required for *two devices of the same
  class* (two keyboards, two MIDI, grid + a second serial device) and is
  therefore a lower-priority, optional phase.

## Feasibility verdict (after code audit)

**Feasible, but materially harder than "finish a gated feature."** An adversarial
audit of the hub-mode code paths found that ASF's `USB_HOST_HUB_SUPPORT` is an
**abandoned, never-compiled skeleton**, not working-but-disabled code.

The proof: `uhi_msc.c:315` contains literal invalid C inside the hub block —
`uhi_msc_dev_sel = &uhi_msc_dev[];` (an empty `[]` subscript). A syntax error
survives in a shipped file only because **nobody has ever built this path**.
Treat *every* hub-mode line as unvetted: expect latent bugs well beyond the ones
catalogued below, and budget on-hardware debugging with a USB analyzer.

This is a **"finish + debug abandoned vendor code" project**, which is often
comparable to writing fresh — you inherit someone's incomplete mental model with
zero test coverage. Realistic effort for grid + keyboard: **weeks**, with the
control-pipe address multiplexing (below) as the make-or-break spike. Do that
spike *first*; if it can't be made reliable, stop before investing in the rest.

What genuinely helps (reusable scaffolding):
- The UHC device model compiles to a doubly-linked device tree
  (`prev`/`next`/`hub`/`hub_port`/`power` on `uhc_device_t`) under the flag.
- The UHI callback contract passes `uhc_device_t*` to `install`/`enable`/
  `uninstall` (`uhi.h:66`), so per-device dispatch is possible.
- The enumeration state machine (`uhc_enumeration_step1..14`) is class-agnostic
  and parameterized by the device under enumeration.
- A heap is configured (`link_uc3b0512.lds`: unlimited `__heap_size__`), so the
  `malloc` the hub path needs is available — it's just never *called* today.

## Build status (verified in docker, `dewb/monome-build`)

Phases 0a + 0b are **implemented and build-verified**:
- **Default build (flag off):** clean, exit 0, **byte-identical** firmware
  (`text 0x4a352, data 0x2f48, bss 0x457e4`) — all hub work is inert until the
  flag is set.
- **With-flag build (`USB_HOST_HUB_SUPPORT`):** **compiles and links cleanly**,
  exit 0 (`text 0x4a8b6`, +1380 B of hub code), no warnings beyond a pre-existing
  ASF one (`navigation.c:1620`).

Compiling the never-built path flushed out **8 real bugs**, all now fixed:
1. core `#error` (uhc.c) — un-gated.
2–4. three `#error` markers over functional control-pipe code (usbb_host.c).
5. set-address loop assigned address 1 to every device (uhc.c).
6. `uhc_device_t` struct used its own typedef name for self-pointers in an
   untagged struct — never compiled (uhc.h).
7. teletype's FTDI driver carried its own hub `#error` (uhi_ftdi.c).
8. `ep_ctrl_size` used **uninitialized** in hub mode → would corrupt control-OUT
   transfers (usbb_host.c) — plus `uhi_msc.c`'s `&uhi_msc_dev[]` syntax error and
   a missing `<string.h>`.

**Not yet validated:** runtime behaviour on hardware (enumeration, the
single-`uhc_dev_enum` serialization race, the hub driver's remaining TODO blocks).
Linking ≠ working — see Phase 1 and the hardware-validation caveats.

## Can a later Atmel/ASF version finish it for us? No.

Researched and confirmed — there is no upstream code to harvest:
- The **latest ASF** (`avrxml/asf` master, current Microchip mirror) **still has
  the `#error`**, still lacks `uhi_hub_*` definitions, and has **no `hub` folder**
  under `common/services/usb/class/` (only aoa, cdc, composite, dfu_flip, hid,
  msc, phdc, vendor). Hub support was never completed in any public ASF release.
- The monome devs already proved upgrading is futile (aleph issue #67): they
  diffed asf-3.7.3 (what teletype uses) against the then-latest asf-3.24.3 and
  found *"almost no change"* in the USB host driver.
- No viable drop-in alternative host stack exists for AVR32 (they evaluated and
  rejected LUFA — no usable host stack — plus csud and mbed-src). TinyUSB does
  not target AVR32 either.

**Therefore the only path is to write the missing `uhi_hub` ourselves.** Best
reference: **TinyUSB `src/host/hub.c`** — a clean, complete hub algorithm to read
as the spec and hand-port against the ASF UHC API (cannot be linked here).

The monome devs independently reached this same conclusion in issue #67:
*"all we really need to do is implement the hub class host controller, find up to
2 or maybe 3 devices, and enumerate them"* / *"the path of least resistance is to
just fill in the gaps in that library."* This validates the Phase 0/1 scope.

### Two practical constraints from issue #67 / ASF docs
- **Mixed USB speeds on one tree are not supported by the hardware.** A
  **low-speed boot keyboard behind the same hub as a full-speed grid/MIDI may
  not work.** ⚠️ Verify the target keyboard enumerates at full speed before
  relying on this combo.
- **Use a powered hub.** The module cannot supply bus power for several
  downstream devices.

## Full blocker inventory (verified)

Enabling `USB_HOST_HUB_SUPPORT` trips **9 blockers**, in three tiers:

**Compile-time (immediate):**
1. `uhc.c:58` — hard `#error The USB HUB support is not available in this revision.`
2. `uhi_msc.c:56` — `#error USB HUB support is not implemented on UHI MSC`.
   **MSC is in the live build** (`config.mk:170`, `USB_HOST_UHI` includes
   `UHI_MSC`), so this *will* fire. Must refactor `uhi_msc` for hub mode or drop
   MSC from hub builds.
3. `uhi_msc.c:315` — the `&uhi_msc_dev[]` syntax error (the smoking gun).
4. `uhi_cdc.c:55` (ASF CDC) — `#error`. teletype's serial uses its *custom*
   `uhi_cdc`/`UHI_MCDC`, so confirm the ASF CDC file isn't the one compiled;
   `uhi_hid_mouse.c` / `uhi_vendor.c` / `uhi_aoa.c` carry the same `#error` but
   are not in teletype's UHI list.

**Link-time:**
5. `uhc.c:229,249` — calls `uhi_hub_suspend()` / `uhi_hub_send_reset()`, which
   **do not exist** (no `uhi_hub.c` anywhere). Undefined symbols.

**The hard core — control-pipe address multiplexing (3 unfinished TODOs):**
6. `usbb_host.c:690` — `#error TODO Add USB address in a list` (allocating pipe 0
   for a second device when it's already allocated for the first).
7. `usbb_host.c:801` — `#error TODO the list address must be updated` (freeing the
   shared control pipe on one device's disconnect without killing the others).
8. `usbb_host.c:1318` — `#error TODO check address in list` +
   `uhd_configure_address(0, …)` (re-pointing the single control pipe 0 at the
   right device's address before each control transfer).
   → These three are the crux: the USBB has **one** control pipe that must be
   time-multiplexed across all device addresses. ASF never finished this.

**Architecture gap:**
9. **No downstream-device creation exists.** `uhc_connection_tree()` is only ever
   called with `&g_uhc_device_root` (`uhc.c:1031`); there is a `free(dev)` on
   disconnect (`uhc.c:299`) but **no matching `malloc`** and no caller that
   builds a non-root device. The hub driver you write must `malloc` a
   `uhc_device_t`, link it into the list, set `hub`/`hub_port`, and drive
   `uhc_connection_tree(true, dev)` itself. Also note the free-address search at
   `uhc.c:436` (`while (usb_addr_free++)` from 0) is logically buggy and needs
   rewriting to avoid address collisions.

## Hardware ceiling (sets the scope)

Two shared hardware resources cap the device count. **Neither is fatal for the
target use case, but the pipe count needs datasheet confirmation.**

**Pipes:** The `AVR32_USBB_EPT_NUM = 8` value is an override that applies *only to
UC3A3* parts (`usbb_host.h:55`). For the **UC3B0512** the count comes from the
toolchain `<avr32/io.h>` (not in-repo) and was **not confirmed** by the audit —
the AT32UC3B is believed to expose **7 pipes** (so ~6 for data after the shared
control pipe). ⚠️ **Confirm against the AT32UC3B datasheet before committing** —
if it's lower, the budget tightens fast.

**Control pipe is singular and shared.** Pipe 0 is the *only* control pipe and
must be time-multiplexed across every device address — exactly the unfinished
work in blockers #6–8. This, not the pipe *count*, is the real constraint.

| Device                       | UHI driver | Data pipes        |
|------------------------------|------------|-------------------|
| Grid (FTDI **or** CDC serial)| `uhi_ftdi` / `uhi_cdc` | 2 (bulk IN + OUT) |
| Keyboard (HID)               | `uhi_hid`  | 1 (interrupt IN)  |
| MIDI controller              | `uhi_midi` | 2 (bulk IN + OUT) |
| Hub itself                   | `uhi_hub`  | 1 (interrupt IN, status) |

→ grid + keyboard + hub = 4 data pipes; + MIDI = 6. Fits in 6–7. Cap the device
count rather than chasing a general N-device hub.

**DPRAM is *not* the bottleneck** (correcting an earlier over-estimate). The USBB
shares one ~4 KB DPRAM (`128 << UFEATURES.FIFO_MAX_SIZE`, read at runtime —
4096 B assumed, unconfirmed) across all pipe FIFOs. A realistic small-packet
tally — control 64 B, grid 2×64 B×2 banks = 256 B, keyboard 64 B, MIDI
2×64 B×2 = 256 B, hub ~8 B — is **≈650 B, well under 4 KB**. DPRAM only bites if
a **MSC** device (512 B packets × 2 banks ≈ 1 KB/pipe) is mounted *concurrently*
with performance devices; since MSC is used transiently for scene load/save, gate
it so it isn't active alongside grid/MIDI and DPRAM is a non-issue.

## RAM is not the constraint

96 KB total, ~88 KB usable after stack. Per *additional* device:

| Driver | Struct | Static buffers              | Per extra device |
|--------|--------|-----------------------------|------------------|
| HID    | ~10 B  | 64 B frame + dirty bitfield | ~80 B            |
| MIDI   | ~6 B   | 64 B rx + 64 B tx           | ~135 B           |
| FTDI   | ~10 B  | 64 B rx + 192 B strings     | ~266 B           |
| CDC    | ~10 B  | 64 B rx                     | ~74 B            |
| UHC `uhc_device_t` | +~28 B (hub fields) | + dynamic conf desc (64–512 B malloc) | ~100–550 B |

A grid + MIDI + serial config adds **~0.5–1 KB**. Pipes (7) and the
enumeration rework are the binding constraints, not memory.

## What actually unlocks the feature: the UHC hub layer

The win is **not** in the four class drivers — it is in the UHC core + a hub
driver. Once a hub can be enumerated and downstream devices given addresses,
UHC's per-device `install()` dispatch (`uhc.c:700`, `uhc_uhis[i].install(dev)`)
hands the grid to `uhi_ftdi`, the keyboard to `uhi_hid`, and the MIDI controller
to `uhi_midi` — each claiming its one device. The existing singletons are
sufficient for these mixed-class combos. Address `1` → grid, `2` → keyboard,
etc., all allocated dynamically by the hub-mode path in `uhc.c:431`.

Per-driver multi-instancing (next section) is a *separate, optional* effort that
only matters for duplicate same-class devices.

## The singleton pattern to dismantle (only for same-class duplicates)

All four custom drivers in `libavr32/src/usb/{hid,midi,ftdi,cdc}/` share an
identical single-device pattern, so the refactor has the same shape four times:

```c
static uhi_x_dev_t uhi_x_dev = { .dev = NULL };   // → static uhi_x_dev_t devs[N];

uhc_enum_status_t uhi_x_install(uhc_device_t* dev) {
    if (uhi_x_dev.dev != NULL)                     // → find a free slot,
        return UHC_ENUM_SOFTWARE_LIMIT;            //   else return the limit
    ...
}

void uhi_x_enable(uhc_device_t* dev) {
    if (uhi_x_dev.dev != dev) return;              // → slot lookup by dev (handle
    ...                                            //   is already passed in — easy)
}
```

The **hard part is the transfer callbacks.** `uhd_callback_trans_t`
(`libavr32/asf/.../uhd.h:158`) delivers only `(usb_add_t add, usb_ep_t ep, …)` —
**no device handle.** Today the callbacks read the singleton directly
(`uhi_hid_dev.report`, `uhi_midi_dev.dev->address`, …). They must instead map
`add → slot` via a small lookup.

### Per-driver notes
- **HID (grid)** — cleanest. Per-device `report` buffer is already `malloc`'d.
  Extra work: `hid.c`'s static 64-byte frame buffer + `dirty` bitfield →
  per-instance; `main.c`'s `hid_get_frame_*()` pollers need a device index.
- **MIDI** — `midi.c` holds static rx/tx ring buffers + `midi_connected` /
  `txBusy` flags → per-instance.
- **FTDI / CDC — biggest snag.** `monome.c` wires a **single global serial
  backend** (`serial_read`/`serial_write`/… set to either `ftdi_*` *or*
  `cdc_*`, around `monome.c:220` and `:966`). These are mutually exclusive
  today, not just single-instance. Concurrent serial means reworking that
  indirection into a per-device table. Also `ftdi_get_strings()`
  (`uhi_ftdi.c:263`) **spin-waits** on a control transfer (`ctlReadBusy`) —
  blocking enumeration that doesn't compose with a second device enumerating
  mid-flight.

### Cleanup bug to fix in passing
`uhi_midi.c`, `uhi_ftdi.c`, and `uhi_cdc.c` each `Assert(uhi_x_dev.report != NULL)`
in their uninstall callback — a copy-paste leftover from the HID driver; the
`.report` field does not exist in those structs. Remove these.

## Phased implementation

Each phase is ordered to **compile and be testable on hardware** before the next.
Phases 0–1 deliver the actual feature (mixed-class multi-device). Phase 2 is
optional and only for same-class duplicates.

### Phase 0a — Control-pipe multiplexing (LARGELY DONE — big de-risk)
**Key finding from reading the code:** the control-pipe multiplexing I'd billed
as the make-or-break crux is **already implemented by Atmel**. The three
`#error`s (`usbb_host.c:690/801/1318`) are conservative markers over *functional*
code, not missing logic:
- `uhd_setup_request()` (843) queues control requests FIFO and starts the next
  only when one finishes → control transfers are already **serialized**.
- `uhd_ctrl_phase_setup()` (~1320) already calls
  `uhd_configure_address(0, uhd_ctrl_request_first->add)` to re-point the single
  shared pipe 0 at the head request's device before each transfer.
- `uhd_ep0_alloc()` (688) already allocates pipe 0 **once**, fixed at 64 B, shared.
- `uhd_ctrl_phase_data_in()` (1369) already handles short-packet detection for
  the fixed-64B pipe.
The `#error`s only flag a desired *address-list bookkeeping* refinement (to know
when the last device leaves so pipe 0 can be torn down) — a spike doesn't need it.

**Edits applied (all inside `#ifdef USB_HOST_HUB_SUPPORT`, inert in default build,
verified: docker build clean + byte-identical firmware):**
- `uhc.c:58` core `#error` commented out (un-gated).
- `uhc.c` set-address loop **bug fixed**: the original `while (usb_addr_free++)`
  never entered the loop and assigned address 1 to *every* device (hub +
  downstream collision). Now searches 1..127 for the first address not in the
  device list.
- `usbb_host.c:690/801/1318` — three `#error` markers removed, functional code
  beneath each retained with explanatory comments.

**Still open before a with-flag build links:** Phase 0b (MSC `#error`/syntax
error + downstream device allocation) and the hub driver's TODO blocks. The
genuine remaining risk is **hardware validation** — this code path was never
compiled upstream (the MSC syntax error proved it), so latent bugs are likely;
the multiplexing logic *looks* correct but is unproven on silicon.
- **Gate (when Phase 0b lands):** single grid still enumerates via hub-mode
  paths; then a hub + two devices get distinct addresses and control transfers
  don't corrupt each other.

### Phase 0b — Downstream device lifecycle + MSC (EDITS APPLIED)
Implemented (all `#ifdef USB_HOST_HUB_SUPPORT`, default build verified clean +
byte-identical):
- **New UHC entry point** `uhc_hub_port_change(hub, port, b_plug)` in `uhc.c`
  (declared in `uhc.h`). Device-list ownership stays in `uhc.c` because
  `g_uhc_device_root` / `uhc_connection_tree` are `static` there. On connect it
  `malloc`s a `uhc_device_t`, sets `hub`/`hub_port`, links it after the root
  (linear, NULL-terminated), and calls `uhc_connection_tree(true, nd)`. On
  disconnect it walks the list for `(hub, port)` and calls
  `uhc_connection_tree(false, d)` (unlink + free).
- **NULL-safe unlink** in `uhc_connection_tree`: the original
  `dev->next->prev = …` assumed a circular list and would dereference NULL at
  the list tail; now guarded.
- **MSC kept single-instance:** removed the `uhi_msc.c` `#error`, fixed the
  never-compiled `&uhi_msc_dev[]` syntax error to point `uhi_msc_dev_sel` at the
  singleton. (You never hub-mount two sticks.)
- **Hub driver wired:** `uhi_hub.c` `on_conn_change_cleared()` now calls
  `uhc_hub_port_change()` for both connect and disconnect.
- **Known limitation (hardware-validation TODO):** the status poll re-arms
  immediately, so two devices changing during a single enumeration can race the
  global `uhc_dev_enum`. One-at-a-time plugging is safe; simultaneous power-on
  connects need an "enum outstanding" gate cleared from the enum-complete path.
- **Not yet verified:** a *with-flag* build actually linking — see gate below.
- **Gate:** with-flag build links (no undefined symbols / `#error`s), then on
  hardware: hub + one device behind it enumerates and works.

### Phase 1 — Minimal hub UHI driver (THE milestone)
- A **starting skeleton already exists**: `libavr32/src/usb/hub/uhi_hub.{c,h}`,
  hand-ported from TinyUSB `src/host/hub.c` onto the ASF UHC/UHD API. It is
  wired into the build but **inert** — the whole body is under
  `#ifdef USB_HOST_HUB_SUPPORT`, and `conf_usb_host.h` only adds `UHI_HUB` to
  `USB_HOST_UHI` under the same flag, so it compiles to nothing until Phase 0
  defines it. Registration done: `config.mk` CSRCS + include dir,
  `conf_usb_host.h` include + guarded list entry.
- Port map (TinyUSB → skeleton): `hub_open`→`uhi_hub_install`,
  `hub_set_config`→`uhi_hub_enable`, `config_port_power_complete`→`on_port_power`,
  `hub_xfer_cb`→`on_status_change`, `process_new_status`→`on_port_status`/
  `on_conn_change_cleared`, `hub_port_reset`→`uhi_hub_send_reset`.
- Key porting constraint baked in: ASF control callbacks carry **no `user_data`**,
  so state lives in a per-hub struct found via `get_hub_by_addr(add)` rather than
  threaded through the transfer (as TinyUSB does).
- TODOs left in the skeleton (all Phase-0-dependent): the downstream
  `malloc(uhc_device_t)` + `uhc_connection_tree(true, dev)` hand-off in
  `on_conn_change_cleared` (blocker #9), chaining GET_STATUS until `C_PORT_RESET`
  before firing `reset_cb`, the `bPwrOn2PwrGood` settle delay, disconnect
  teardown, and hub-level (bit 0) events.
- Still to flesh out beyond the skeleton: hub descriptor parse, per-port
  power-on, port status/reset, and the status-change interrupt endpoint that
  drives downstream connect/disconnect → `uhc_connection_tree()`. Use TinyUSB's
  `src/host/hub.c` as a *behavioral reference* (do not link it).
- Register `UHI_HUB` in the `conf_usb_host.h` `USB_HOST_UHI` list (it joins
  `uhc_uhis[]` at `uhc.c:130`).
- **Goal:** plug a hub with **grid + keyboard** (or grid + MIDI). Both work
  simultaneously using the *existing* `uhi_ftdi` + `uhi_hid` singletons. This is
  the feature the request is really about.
- **Watch:** `monome.c` sets the global serial backend pointers once at grid
  connect (`monome.c:220`). With only one serial device that is fine; just
  ensure hub/keyboard connect events don't clobber that wiring.

### Phase 2 — Multi-instance a single class (optional, for duplicates only)
Needed only if the user wants *two of the same class* at once. Per the
"singleton pattern" section: convert the chosen driver's `static …_dev` to an
array, replace the `if (dev != NULL) return SOFTWARE_LIMIT` check with free-slot
search, and add an `add → slot` lookup inside the transfer callback (which lacks
a device handle).
- **HID (two keyboards):** also de-singleton `hid.c` frame/`dirty` state and
  index the `main.c` pollers by device. Low real-world demand.
- **MIDI (two controllers):** array-ify `uhi_midi` + `midi.c` rx/tx state.
- **Serial (grid + second serial device) — hardest:** rework `monome.c`'s global
  serial function pointers into a per-device table **and** make
  `ftdi_get_strings()` non-blocking (it currently spin-waits on `ctlReadBusy`,
  which does not compose with concurrent enumeration). Defer unless required.

## Cross-cutting risks
- **Pipe exhaustion** — enforce the device cap; surface a clear "too many USB
  devices" state rather than failing enumeration silently.
- **Power budgeting** — `conf_usb_host.h` advertises 500 mA; hub + multiple
  devices can exceed the module's available bus power. The ASF `power` field is
  scaffolded but unenforced.
- **On-hardware USB debugging** is the real cost sink — budget for a USB
  analyzer (e.g. Beagle / Wireshark capture) when enumeration misbehaves.
- **MSC** (`uhi_msc.c`, USB-stick scene storage) also carries a hub `#error`. If
  it's compiled in the target build, it needs the same multi-instance treatment
  or must be explicitly excluded from hub paths.

## Key file reference

| Path | Role |
|------|------|
| `libavr32/conf/conf_usb_host.h` | UHI driver list, power, `USB_HOST_HUB_SUPPORT` |
| `libavr32/asf/common/services/usb/uhc/uhc.c` | Core enum + device tree; the `#error` at :58 |
| `libavr32/asf/common/services/usb/uhc/uhi.h` | UHI callback contract (passes `uhc_device_t*`) |
| `libavr32/asf/avr32/drivers/usbb/usbb_host.{c,h}` | USBB HW driver; 8-pipe limit; HW hub TODOs |
| `libavr32/src/usb/hub/{uhi_hub}.{c,h}` | **NEW** hub driver skeleton (inert until the flag) |
| `libavr32/src/usb/hid/{uhi_hid,hid}.c` | Keyboard (HID) driver + frame layer |
| `libavr32/src/usb/midi/{uhi_midi,midi}.c` | MIDI driver + rx/tx buffers |
| `libavr32/src/usb/ftdi/{uhi_ftdi,ftdi}.c` | FTDI serial driver (blocking string fetch) |
| `libavr32/src/usb/cdc/{uhi_cdc,cdc}.c` | CDC serial driver |
| `libavr32/src/monome.c` | Grid enumeration + global serial backend wiring |
| `module/config.mk` | Build file list (add `uhi_hub.c` here) |
