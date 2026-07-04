# Plan: Native Meadowphysics mode in Teletype

Preliminary implementation plan for porting Ansible's Meadowphysics (MP) sequencer
into Teletype firmware as a native mode driving Teletype's own CV/TR outputs.

Source codebase: `~/Claude/ansible` (MP lives in `src/ansible_grid.c`, ~lines 3510–4620).
Target codebase: `~/Claude/teletype`.

## Goal & confirmed decisions

| # | Decision | Resolution |
|---|---|---|
| — | Integration approach | **Native port** — new `M_MEADOWPHYSICS` mode running the MP engine inside TT firmware (not i2c control of an external Ansible) |
| 1 | Output ownership | **Exclusive** — MP is sole writer of the 4 CV + 4 TR while in MP mode; script output suppressed |
| 2 | Voice-mode → outputs | **Resolved** — see mapping table below |
| 3 | Persistence | **Per-scene** — `mp_data_t` stored in `scene_state_t` + scene serializer (32 TT scene slots); drops MP's native 8-preset concept |
| 4 | Clock | **Separate** — MP runs its own internal tempo via dedicated timer, independent of TT's `M` metro; optional external clock on **Tr input 1** (default internal) |
| 5 | Grid size | **16×8** default (16 cols × 8 rows); top-8-rows fallback on 16×16, clamped fallback on 8×8 |
| 6 | Operator input model | **Resolved by USB hub** — grid = sequencer editing surface; **USB keyboard** = view-switching + transport + config (replaces Ansible's 2 front buttons); **TT ops** = scriptable/headless control (native `MP.*`). All three run concurrently (see below). TT's single front button and param knob are *not* needed for MP. |

## What Meadowphysics is

A **cascading-counter sequencer**: 8 rows, each an independent counter with
`count / speed / min / max`. When a counter rolls over it fires its
**triggers/toggles**, optionally **syncs** other rows, and applies a **rule**
(`inc / dec / max / min / rnd / pole / stop`) to a destination row's count and/or
speed. Supports 4 voice modes (`1V / 2V / 4V / 8T`) and a scale.

- Data model: `mp_data_t` (109 B) in `ansible/src/ansible_grid.h:120-140`.
- Engine: `clock_mp()`, rule logic, `mp_note_on/off`, `get_note_slot`, `calc_scale`
  in `ansible/src/ansible_grid.c:3510-4620` (~1,100 lines; self-contained, hardware-abstracted).

## Architecture mapping (Ansible → Teletype)

| Concern | Ansible (source) | Teletype (target) |
|---|---|---|
| App selection | `set_mode_grid()` + handler vtable | new `M_MEADOWPHYSICS` in `tele_mode_t` (`module/globals.h:25`) + case in `set_mode()` (`module/main.c:766`) |
| Grid key | `handler_MPGridKey` | new `mp_grid_process_key()`, dispatched from `grid_process_key()` (`module/grid.c`) |
| Grid LEDs | `refresh_mp` → `monomeLedBuffer` | new `mp_grid_refresh()` writing `monomeLedBuffer` (`buf[y*16+x]`, single quadrant) |
| OLED screen | none (Ansible has no display) | **new** `screen_refresh_meadowphysics()` — net-new design |
| Clock | `clock_mp(phase)` via `clock` ptr | dedicated `timer_add()` at MP period + external clock via a TR input |
| Sequencer step | `clock_mp` | port near-verbatim into engine `mp_clock()` |
| CV/TR out | `set_cv_note` / `set_tr` / `clr_tr` / `dac_set_value` | `tele_cv()` / `tele_tr()` (`src/teletype_io.h:25-31`) |
| Front-panel keys | `handler_MPKey` | `process_meadowphysics_keys()` |
| Persistence | global `f.mp_state` (8 presets) | per-scene `mp_data_t` in `scene_state_t` (`src/state.h`) + `scene_serialization.c` |
| Scales | `scale_data[16][8]`, `calc_scale` | reuse TT scale tables / note→CV path |

## Decision #2 — voice-mode → 4 CV + 4 TR (locked)

In `mp_note_on(n)`, `n` is the counter **row** (0–7); `voice_mode` decides output mapping.
Pitch = `cur_scale[7-n] + scale_adj[7-n]` (row 7 = lowest). `mp_clock_count` caps notes
per tick; `note_now[4]`/`note_age[4]` do voice allocation + oldest-stealing.

Committed Teletype mapping (`tele_cv(i,v,slew)` / `tele_tr(i,level)`, 0-based):

| Mode | Notes/tick | Teletype outputs |
|---|---|---|
| **1V** (mono) | 1 | `tele_cv(0, pitch, 0)` + `tele_tr(0, 1/0)` |
| **2V** | 2 (stolen) | `tele_cv(w, pitch, 0)` + `tele_tr(w, 1/0)`, `w∈{0,1}` |
| **4V** | 4 (stolen) | `tele_cv(w, pitch, 0)` + `tele_tr(w, 1/0)`, `w∈{0..3}` |
| **8T** (8 gates) | 8 | rows 0–3 → `tele_tr(row, 1/0)`; rows 4–7 → `tele_cv(row−4, FULL/0, 0)` |

- **8T** reproduces Ansible's trick of repurposing the 4 CV jacks as 0/10V gates to get
  8 gate outputs from 4+4 I/O. CV/TR pairs are index-locked (`CV[w]` ↔ `TR[w]`).
- Gates are **held levels** (set on note-on, cleared on note-off), not fixed pulses —
  use `tele_tr(i, level)`, NOT `tele_tr_pulse`. Engine keeps deciding gate timing.

### Output-binding notes (Phase 2) — ✅ RESOLVED & IMPLEMENTED
Implemented in `src/meadowphysics_binding.{c,h}` (`mp_binding_output()` vtable + `mp_note_to_cv()`).
1. **8T full-scale constant — RESOLVED.** `MP_CV_FULL = 16383`. Confirmed `tele_cv(i, v, s)`
   (`module/main.c:1077`) takes a **raw 14-bit value**, adds the per-channel calibration offset
   `aout[i].off`, and clamps to `16383` — same path the `CV`/`N` ops feed. So the binding passes
   raw ET values; calibration is applied downstream. Gate on → `tele_cv(ch, 16383, 0)`, off → `0`.
2. **Pitch conversion — RESOLVED.** Engine emits a scale degree (`cur_scale[7-n]+scale_adj[7-n]`)
   to `out.cv`; `mp_note_to_cv()` converts it via libavr32's `ET[]` equal-temperament table —
   byte-identical to the `N` op's `note_number_to_volts()` (`maths.c:337`, where `table_n == ET`).
   Clamped to ±127; `scale_adj` carries over. Unit-tested against `ET` (`binding_note_to_cv`).
3. **Gates are held levels** — `tele_tr(ch, on)` / `tele_cv(ch, level, 0)`, not `tele_tr_pulse`;
   the engine keeps deciding gate timing (decision #2).
4. **CV resting state on exit** — default: leave CV at MP's last value; gates forced low.
   (Applied by the mode shell on exit, Phase 4.)

## Open sub-question (not a blocker)
- **16 columns = 16 counter steps?** The row→output mapping (above) is independent of grid
  width. Whether the wider 16-col grid extends counter range (`count/min/max`) from 8 to 16
  lives in `handler_MPGridKey`/`clock_mp` — verify Ansible's 16-wide behavior during Phase 1.

## Decision #6 — operator input model (locked, enabled by USB hub)

The USB host **hub support** (commit `3d64d42`) lets a monome grid (CDC-class) and a USB
keyboard (HID-class) enumerate **at the same time** — different device classes route to
different single-instance UHI drivers, so grid+keyboard is well inside the 3-device pipe
ceiling. This dissolves the hardware mismatch that made the MP operator layer awkward: Ansible
drives MP with **2 front buttons** (view switching) + grid, but TT has only **1** front button.
We no longer need to overload it — the keyboard supplies the buttons, and ops supply a
scripting path Ansible never had.

**MP has no rotary knob.** In Ansible every MP parameter (tempo, voice mode, scale, rules) is
grid-edited; the front buttons only switch views and the Tr jacks do clock/reset. So TT's param
knob is irrelevant to MP, and "knob functions" reduce to **keyboard keys or ops**.

### Three concurrent input surfaces

| Surface | Role | Maps from Ansible |
|---|---|---|
| **monome grid** | Sequencer editing surface — counters, ranges, speeds/triggers, rules, config views | `handler_MPGridKey` (ported ~verbatim) |
| **USB keyboard** | View switching, transport (start/stop/reset), voice-mode/scale/tempo entry, mode enter/exit | `handler_MPKey` (2 front buttons) → keys |
| **TT ops** | Scriptable + headless control: reset/stop/start, view, tempo, voice-mode from scripts | net-new; `handler_MPTr` reset ≈ `MP.RESET` |

### Keyboard bindings (proposed, confirm during Phase 4/5)
- **Enter/exit MP**: dedicated global binding in `process_global_keys()` (`main.c:852`), e.g.
  `alt-M`, mirroring how `alt-H` toggles help. MP is independent of `grid_control_mode`.
- **View switch** (Ansible's 2 buttons → 3 views): number keys `1`/`2`/`3` = Positions /
  Clock+Tempo / Config, or `Tab` cycles within MP. Within Positions, sub-edit (positions /
  speed+trig / rules) on a key toggle.
- **Transport**: e.g. `Space` = start/stop, `R` = reset (Ansible's Tr[3] reset).
- **Config entry**: voice mode + scale selectable from the keyboard in Config view (also grid).

### Op bindings (native)
- Reuse the **existing `MP.*` op names** to drive the **native** engine (see A5): `MP.RESET`,
  `MP.STOP`, `MP.PRESET`; consider adding `MP.START`, `MP.TEMPO`, `MP.VOICE`, `MP.VIEW`.
- Precedent for ops mutating module mode state: `op_LIVE_OFF/DASH/GRID` → `set_live_submode()`,
  `grid_key_press()`, `device_flip()`, all via `teletype_io.h` callbacks. Add a
  `set_meadowphysics_*()` callback in the same style rather than exporting module statics.

### Dispatch wiring
- Keyboard: add `case M_MEADOWPHYSICS: process_meadowphysics_keys(key, mod_key, is_held_key);`
  to the mode switch in `process_keypress()` (`main.c:836`). Signature matches every other
  `process_*_keys`. Modifier helpers in `keyboard_helper.h` (`match_alt`, `match_shift`, …).
- Ops: `src/ops/meadowphysics.c` calls new `teletype_io.h` callbacks implemented in `main.c`.

## Phased implementation

0. **NVRAM baseline + measurement gate — ✅ BASELINE MEASURED.** Clean AVR32 build
   (`docker run --rm --platform linux/amd64 -v "$(pwd)":/target dewb/monome-build 'cd module &&
   make clean && make'`, exit 0) on `usb-hub-support` @ `3d64d42`. Verified `.flash_nvram` from
   both `module/teletype.map` and `avr32-size -A`:
   - **Baseline `.flash_nvram` = 198,448 B (0x30730)** of the 204,800 B / 200 KB section ⇒
     **96.9% full, 6,352 B headroom.** (`.text` = 227,652 B ⇒ ~90 KB code headroom — non-issue.)
   - Linker already enforces `ASSERT(.data does not overflow into .nvram)` as a hard backstop.

   **Gate rule for the rest of the port:** re-measure `.flash_nvram` after any change to
   `nvram_scene_t`/`scene_state_t` and **fail the build if it exceeds ~202,000 B** (safety margin
   below the 204,800 B ceiling). Persist `mp_data_t` only (~105 B/scene ⇒ expected +~3.4 KB →
   ~201,936 B); if the measured delta is larger, ephemeral runtime state has leaked into the
   serialized scene — fix before proceeding. If headroom is exhausted, invoke the `tele_data_t`
   8→4 B escape valve (see Footprint) as a prerequisite, not an afterthought.
1. **Engine extraction — ✅ DONE.** Ported `clock_mp`, the rule switch, `mp_note_on/off`,
   `get_note_slot`, `calc_scale`, `default_mp` into `src/meadowphysics_engine.{c,h}`. Bare
   globals → `mp_engine_t` (`cfg` persistent + `rt` runtime, cleanly split per the Phase 0 NVRAM
   rule — resolves B2 namespacing); hardware calls → `mp_output_t` vtable (`tr`/`cv`/`cv_gate`);
   `rnd()` injected for deterministic tests. **13 unit tests** in `tests/meadowphysics_tests.c`
   (countdown period, speed divider, all 6 rules incl. wrap/pole/stop, 8T + 1V routing,
   calc_scale, 16-step range) — **73/73 suite green, clang-format clean.**
   - **16-step question RESOLVED:** two distinct "8"s — **8 counters/rows** (structural, fixed
     `MP_ROWS`) vs the **count/min/max range** (data-driven `uint8_t`, set across grid width by
     the UI). The engine caps nothing, so a 16-wide grid gives a 16-step range with zero engine
     change; Ansible's own defaults (`count[i]=7+i` → 7–14) already assume ≥15-wide. Test
     `range_supports_16_steps` locks this in. Grid width → range ceiling is a Phase 5 (UI) concern.
2. **Output binding — ✅ DONE.** `src/meadowphysics_binding.{c,h}`: `mp_binding_output()`
   vtable routes engine `tr`/`cv`/`cv_gate` events to `tele_tr`/`tele_cv`; `mp_note_to_cv()`
   converts scale degrees via `ET[]` (identical to the `N` op). 8T CV-as-gate uses
   `MP_CV_FULL = 16383` (confirmed raw-14-bit `tele_cv` units). 2 unit tests; engine stays
   libavr32-free (ET dependency isolated to the binding). See resolved notes above.
3. **Clock — ✅ CORE DONE (firmware wiring → Phase 4).** `src/meadowphysics_clock.{c,h}`:
   hardware-abstract tempo + source arbitration, host-tested (3 tests). Reproduces Ansible's
   model — a timer fires every `period` ms and **toggles phase 0↔1** each fire
   (`ansible/src/main.c:138`), so `period` is the edge interval and a full on/off cycle is two
   fires. `mp_clock_internal_fire()`/`mp_clock_external_edge()` implement "only the active source
   advances"; `mp_clock_period_from_rough_fine()` ports the `20 + rough*16 + fine` tempo encoding
   (clamped [20,265]). **Firmware wiring deferred to Phase 4** (needs the engine instance +
   mode-active flag): a `softTimer` (`timer_add`/`timer_remove` in enter/exit) whose callback runs
   `mp_clock_internal_fire()` → `mp_engine_clock(phase)`, and a `handler_Trigger` hook that calls
   `mp_clock_external_edge()` for **Tr input 1** (A3).
   - **External-clock granularity RESOLVED (Phase 4):** `handler_Trigger` fires on **both** edges
     (it reads the pin state and branches rising/falling), so the external clock is **gate-driven**
     — pin high → phase 1 (step + notes), pin low → phase 0 (clear); one pulse = one step. The
     clock module's external API takes the pin `level` (not a toggle). Gate width sets note-off
     timing. (Bench-validate feel on hardware; the model is correct by construction.)
4. **Mode shell + input surfaces (per #6) — ✅ DONE (firmware builds clean, exit 0).**
   `module/meadowphysics_mode.{c,h}`: owns the `mp_engine_t` + `mp_clock_t` instances and the
   dedicated clock `softTimer` (added/removed on enter/exit); `set_meadowphysics_mode()`,
   `meadowphysics_mode_exit()`, `process_meadowphysics_keys()`, `screen_refresh_meadowphysics()`
   (basic status view; full design Phase 6). Wired into `main.c`: `M_MEADOWPHYSICS` in `globals.h`
   enum; `set_mode` case; key-dispatch case; `alt-M` toggle in `process_global_keys`;
   `handler_ScreenRefresh` case; `handler_AppCustom` routes `data==1` → `meadowphysics_clock_tick`
   (metro guarded to `data==0`); `handler_Trigger` external-clock hook for Tr `MP_EXT_CLOCK_INPUT`.
   **Exclusive output ownership:** `meadowphysics_suppresses_output()` gates `tele_tr`/`tele_cv`
   (MP active + not mid its own write); exit forces gates low, leaves CV. Keys: `1/2/3` views,
   `Space` run/stop, `R` reset, `V` cycle voice mode, `X` toggle external clock, `-`/`=` tempo.
   Ops retarget (A5) → Phase 8 or a follow-up. `config.mk` updated with all 4 new `.c` files.
   - ⚠️ **Flash capacity resolved here:** MP overflowed flash by ~2.2 KB (NVRAM sits at the top,
     leaving only ~1.6 KB code headroom). Fix: **`SCENE_SLOTS` 32 → 30** (`flash.h`) + NVRAM
     region **200K → 190K** (`config.mk`). Verified sizes: `.text` 231,056; `.flash_nvram` 186,064
     (of 190K ⇒ 8.5 KB slack, survives Phase 7); code headroom ~8.5 KB for Phases 5/6/8. Note:
     this changes the on-flash `nvram_data_t` layout — existing scenes reinit (folds into the
     Phase 7 version bump).
5. **Grid UI — ✅ DONE (firmware builds clean, exit 0).** `src/meadowphysics_grid.{c,h}`
   (host-testable, 5 tests): ports Ansible's `handler_MPGridKey` NORMAL branch + `refresh_mp` into
   `mp_grid_process_key()` / `mp_grid_refresh()` operating on the engine + an `mp_grid_state_t`
   (edit sub-mode, edit_row, kcount, per-row `scount`) and a raw 128-byte LED buffer. 16×8: 8 rows
   = counters, 16 cols = range. Three sub-modes (positions / speed+trig / rules) switched by
   holding **grid col 0 → speed, col 1 → rules** (faithful to Ansible; the OLED-level clock/config
   views remain keyboard-driven per #6). Multi-press count/range setting, trigger/toggle/sync,
   speed range, and rule glyphs all ported; mono-grid varibright fallback (B5). Wired into
   `grid.c`: `G_MEADOWPHYSICS` enum, `M_MEADOWPHYSICS→G_MEADOWPHYSICS` in `grid_set_control_mode`,
   and **early intercepts in `grid_process_key`/`grid_refresh`** so MP *owns the whole grid* while
   active (via `meadowphysics_active()`), bypassing the scene-grid surface. `handler_AppCustom`
   marks `grid.grid_dirty` each tick so the grid tracks the sequencer. `.text` 232,588 (+1.5 KB).
   - ⚠️ **Grid rotation** (`SG.rotate`) is not applied to the MP surface (intercept returns before
     the rotation pass) — fine for standard-orientation grids; note for a follow-up if needed.
6. **OLED screen — ✅ DONE (builds clean, exit 0).** Net-new TT-style view in
   `screen_refresh_meadowphysics()` (`meadowphysics_mode.c`), replacing the Phase 4 placeholder.
   Persistent header (L0–L3): title + view tabs (P/C/F, active bright), voice mode + RUN/STOP,
   clock INT/EXT + period, grid sub-mode + scale. View-specific detail panel (L4–L7) driven by the
   keyboard `1/2/3` views (gives them real OLED effect): **Positions** → selected-row detail
   (count / range / speed / rule / dest+target); **Clock** → source, period, derived steps-per-min;
   **Config** → voice mode, scale, key hints. 6px font (21 cols), brightness label/value/title =
   5/12/15. Complements the grid (which shows the counters visually) with the numbers + rule wiring
   it can't. `.text` +0.75 KB. Screens aren't host-testable; verified by compile + design review.
7. **Persistence — ✅ DONE (builds clean, exit 0; Phase 0 gate passes).** `mp_config_t` added to
   `scene_state_t` (`state.h`) and `nvram_scene_t` (`flash.h`); `flash_read`/`flash_write` memcpy it
   both directions (`flash.c`); `ss_mp_init()` (`state.c`, called from `ss_init`) sets defaults for
   fresh scenes (B1). `voice_mode`/`sound` **moved from `mp_runtime_t` into `mp_config_t`** so a
   scene captures the whole sequencer setup (engine/grid/tests updated; 83/83 still green).
   **Sync:** load `scene_state.mp → mp_eng.cfg` on MP enter (re-arm only if the scene changed, via
   `memcmp`, so a quick toggle doesn't restart a running sequence); write `mp_eng.cfg → scene_state.mp`
   on exit (you always leave MP before the preset-write save). **Version:** `FIRSTRUN_KEY`
   0x22→0x23 forces a clean reinit (the nvram layout changed via SCENE_SLOTS + the mp field; no
   per-scene migration path exists — consistent with the accepted scene reinit).
   **Phase 0 gate re-applied:** `.flash_nvram` 186,064 → **189,064** (+3.0 KB, 30 slots × ~100 B);
   190 KB region ⇒ **5,496 B slack**, under the safety line. ✓
   - ⚠️ **Text serialization (`#MP` section in `scene_serialization.c`) deferred** — the USB
     text export/import path doesn't yet round-trip MP config (flash save/load, the primary
     persistence, does). It's additive + backward-compatible (old firmware ignores unknown
     sections; new firmware defaults MP for old text scenes), so it can land later without
     breaking anything. Flagged so a text round-trip's silent MP-loss is a known gap, not a surprise.
   - **Clock tempo/source not persisted** (lives in `mp_clk`, not `mp_config_t`) — a scene restores
     its sequencer config but not its tempo; deliberate scope boundary, notable for a follow-up.
8. **Build/registration + docs/help — ✅ DONE (firmware/sim/tests all green).**
   - **A5 ops retargeted to native:** `src/ops/meadowphysics.c` — `MP.RESET`/`MP.STOP` now drive
     the native engine via new `teletype_io.h` callbacks (`meadowphysics_op_reset/stop`, 0=all,
     1-8=row); `MP.PRESET` is a documented no-op (no presets on TT MP). Same names/arity ⇒ **no
     enum regen, no `match_token.rl`/`op.c` change** (verified `op_enum.h` unchanged). Callbacks
     implemented in the mode file; stubbed in `tests/main.c` + `simulator/tt.c`.
   - **Help:** new page 18 "MEADOWPHYSICS" in `help_mode.c` (keys, grid, outputs, ops); bumped
     `HELP_PAGES` 17→18 and all page-header totals `/17`→`/18`.
   - **Docs:** `docs/ops/meadowphysics.{toml,md}` rewritten for the native mode (overview, keys,
     grid, voice-mode outputs, per-scene persistence, retargeted ops).
   - **Build fix:** `simulator/Makefile` needed `meadowphysics_engine.o` (shared `state.c`'s
     `ss_mp_init` pulls in `mp_engine_set_defaults`) — the earlier "sim n/a" note was wrong.
   - Verified: AVR32 build exit 0 (`.text` 233,696, ~5.8 KB headroom); 83/83 host tests;
     simulator builds + `MP.RESET` parses; clang-format clean on changed lines.

## Files

**New**
- `src/meadowphysics_engine.{c,h}` — ported sequencer engine (hardware-abstracted) ✅
- `src/meadowphysics_binding.{c,h}` — output binding: engine events → `tele_cv`/`tele_tr`, ET pitch ✅
- `src/meadowphysics_clock.{c,h}` — tempo model + internal/external phase arbitration ✅
- `src/meadowphysics_grid.{c,h}` — grid key handling + LED rendering (16×8) ✅
- `module/meadowphysics_mode.{c,h}` — TT mode shell (keys, screen, output binding, clock) ✅
- `tests/meadowphysics_tests.c` — engine/binding/clock unit tests (18 tests) ✅

**Modified**
- `module/globals.h` — `M_MEADOWPHYSICS` in `tele_mode_t` ✅
- `module/main.c` — include, `set_mode` case + exit hook, key/screen dispatch, `alt-M`,
  `handler_AppCustom` (data==1) + `handler_Trigger` clock hooks, `tele_tr`/`tele_cv` ownership gate ✅
- `module/flash.h` / `flash.c` — `SCENE_SLOTS` 32→30, `mp` in `nvram_scene_t`, memcpy load/save,
  `FIRSTRUN_KEY` 0x22→0x23 ✅
- `module/config.mk` — 5 new `.c` files in `CSRCS` ✅; `__flash_nvram_size__` 200K → 190K ✅
- `module/grid.c` — `G_MEADOWPHYSICS` enum, mode mapping, key/refresh intercepts (MP owns grid) ✅
- `src/state.h` / `state.c` — `mp_config_t mp` in `scene_state_t`; `ss_mp_init()` ✅
- `src/scene_serialization.c` — `#MP` text section (deferred; additive/backward-compatible)
- `src/ops/meadowphysics.{c,h}` — `MP.*` ops retargeted to native engine (A5) ✅
- `src/teletype_io.h` — `meadowphysics_op_reset/stop` callbacks ✅
- `module/help_mode.c` — Meadowphysics help page (18) ✅
- `docs/ops/meadowphysics.{toml,md}` — native-mode docs ✅
- `tests/Makefile` + `tests/main.c` — MP objects + op stubs ✅
- `simulator/Makefile` + `simulator/tt.c` — `meadowphysics_engine.o` + op stubs ✅

## Footprint impact (verified against linker map + struct sizes)

Three budgets, only one is tight. Numbers below are from the actual build (`module/teletype.map`)
and `sizeof` of the ported structs.

### Code flash — ⚠️ THE ACTUAL WALL (corrected in Phase 4)
- 512 KB total. The 200 KB `.flash_nvram` region is placed at a **fixed address at the top of
  flash** (`ORIGIN+LENGTH-200K` = 0x8004e000) and ends **exactly at the 512 KB limit**. So
  code+rodata+`.data` must fit *below* 0x8004e000, and the linker `ASSERT` enforces it.
- **Measured pre-MP: code+data ends at offset 317,856 ⇒ only 1,632 B free below NVRAM** (the
  earlier "~90 KB headroom" was wrong — that was distance to flash-end, but NVRAM occupies it).
- **MP adds 3,783 B of flash ⇒ overflows by ~2,151 B.** Build fails: `.data overflowed into
  .nvram!`. All MP files compile clean; the *only* failure is capacity.
- Phase 7 compounds it: +3,488 B NVRAM content. Full port needs ~7.3 KB beyond the 1.6 KB free.
- **✅ RESOLVED (Phase 4): `SCENE_SLOTS` 32 → 30 + NVRAM region 200K → 190K.** Dropping 2 scene
  slots frees ~12.4 KB of NVRAM content (198,448 → 186,064), so the 190K region holds it with
  8.5 KB slack (survives Phase 7). Shrinking the region moves its fixed start up 10 KB, giving
  program flash room: post-MP `.text` = 231,056 with ~8.5 KB headroom for Phases 5/6/8. Build
  links clean. Trade-off accepted by user: 32 → 30 user scene slots; on-flash layout change
  reinitializes existing scenes (folds into Phase 7's format version bump).

### SRAM — comfortable ✓
- 64 KB total; `scene_state` (single RAM instance) is the dominant consumer at ~29 KB (~44%),
  with ~35 KB free.
- MP adds: persistent `mp_data_t` = **105 B** (12×`u8[8]` + `scale` + `glyph[8]`; drop `glyph`
  ⇒ ~97 B since #3 has no presets) + ephemeral runtime state (`position/tick/pushed/…`) ≈ **84 B**.
- Total ≈ **~190 B** on 35 KB free — negligible (<0.3%).

### NVRAM scene storage — THE binding constraint ⚠️ (fits, but tight)
- The `.flash_nvram` section is a **fixed 200 KB** linker allocation holding 32 scene slots.
  Scenes are stored as a **raw `memcpy`** of `nvram_scene_t` (`flash.c:79`), **not** a packed
  format — so anything added to the per-scene struct multiplies by 32.
- **Current usage: 198,448 B / 204,800 B = 96.9% full — only ~6.3 KB headroom before MP.**
- Adding `mp_data_t` (109 B) × 32 slots = **+3,488 B** ⇒ **201,936 B (98.6%)**, leaving
  **~2.9 KB margin**. It fits, but MP consumes **over half** the remaining slack.
- **Load-bearing design rule:** persist **only `mp_data_t`** (~105 B). The ~84 B runtime state
  must live outside the serialized scene (module runtime / RAM-only fields), or flash growth
  nearly doubles. Dropping `glyph[8]` (no presets) reclaims 256 B across the 32 slots.
- **Escape valve if margin is insufficient / for future headroom:** the memory analysis
  identifies shrinking `tele_data_t` 8→4 B, which reclaims **~10 KB** across the script arrays
  that dominate each scene — far more than MP costs — but it's serialization-sensitive and a
  separate effort. Not required for MP; noted as the lever if the 2.9 KB margin proves too thin.

## Risks / watch-items
- **OLED screen is net-new** (no source to port) — design effort, not translation.
- **Clock resolution/jitter** — TT timer granularity vs MP's expected step rate; dedicated
  timer should give finer control than reusing the 10-tick `RATE_CLOCK`.
- **NVRAM budget is the real ceiling (⚠️ highest-attention item)** — the scene section is
  already **96.9% full**; MP fits with only **~2.9 KB margin**. Persist `mp_data_t` only (never
  the runtime state), and re-check the linker map (`.flash_nvram`) on the AVR32 build as a hard
  gate in Phase 7. Do not let any other per-scene addition land alongside MP without re-measuring.
- **Serialization versioning** — because scenes are a **raw `memcpy`** of `nvram_scene_t`,
  inserting `mp_data_t` changes the on-flash byte layout; existing user scenes **will not load**
  without a version bump + migration path (default-init MP fields for old scenes). Coordinate the
  flash struct change with `flash.h`/`flash.c` and the text import/export in
  `scene_serialization.c`.
- **libavr32 parity** — both repos vendor `libavr32`; confirm scale/music tables and
  `monomeLedBuffer` semantics match (they appear to).
- **Exclusive-ownership suppression** — confirm the exact point where script `tele_cv/tele_tr`
  writes are gated off while in MP mode (Phase 4).
- **Keyboard+grid concurrency depends on the hub build** — the #6 input model (grid editing +
  keyboard views at once) requires firmware built with `USB_HOST_HUB_SUPPORT`; without it the
  host enumerates one device. MP must still be usable **grid-only** (or **keyboard/op-only**,
  headless) so a non-hub build degrades gracefully rather than becoming undriveable. Also mind
  the **3-device pipe ceiling** (grid + keyboard + MIDI) when combining with other USB gear.

---

## Completeness review

A review of this plan against the codebase maps. The engine port, #2 output mapping,
persistence shape, architecture, and footprint sections are complete. The operator/UX gaps
below clustered around driving MP on TT hardware; the USB hub + keyboard + ops (decision #6)
resolved them. **All of A1–A6 are now locked** — no open decisions remain in this section.

### A. Gaps needing a decision → all resolved

- **A1 — Mode entry/exit. → RESOLVED by #6.** Dedicated keyboard binding in
  `process_global_keys()` (`main.c:852`), e.g. `alt-M`, mirroring `alt-H` for help. MP mode is
  **independent of `grid_control_mode`** (the front-button toggle stays as-is). No front-button
  overloading needed now that a keyboard coexists with the grid.
- **A2 — MP sub-mode navigation. → RESOLVED by #6.** Ansible's 2 front buttons that switch the
  3 views (`handler_MPKey`) map to **keyboard keys** (number row `1/2/3` or in-mode `Tab`),
  handled in `process_meadowphysics_keys()`. The wider key set removes the multi-hold gymnastics
  Ansible needed; sub-edit within Positions view on a key toggle.
- **A3 — External clock input. → LOCKED.** Default **internal** clock. When external clock is
  enabled, MP consumes **Tr input 1** (`IN`/trigger 1). Reset/transport come from keyboard/ops,
  so no second input is needed. Watch conflict with script triggers on that input
  (`handler_Trigger`, `main.c:497`) — while MP external-clock is active, MP owns Tr 1.
- **A4 — Scale source. → LOCKED.** Reuse **TT's scale system** / note→CV path. Do **not** port
  or duplicate MP's `scale_data[16][8]` into the scene — keeps per-scene state small and pitch
  on TT calibration.
- **A5 — `MP.*` ops. → LOCKED.** Retarget the existing `src/ops/meadowphysics.c`
  `MP.PRESET/RESET/STOP` to drive the **native MP port** (not external i2c Ansible), via new
  `teletype_io.h` callbacks (`op_LIVE_*` style); add `MP.START/TEMPO/VOICE/VIEW` as needed.
  External-Ansible i2c control is dropped from these ops. Resolve the `op_MP_*` C-symbol reuse
  during implementation.
- **A6 — Feature scope. → LOCKED.** In scope:
  - **Manual play (`sound` mode)** — grid position taps fire notes live (`pushed[y]`,
    `ansible_grid.c:4232/4277`). Port as-is; also expose the `sound` toggle to keyboard/op.
  - **Per-row stop/start + push** — individual row stop/restart (`position[y] = -1`,
    `:4280`) and manual push. Global **reset** is `MP.RESET` / a keyboard key (replaces Ansible's
    Tr[3] reset), not a second input.
  - **Config editor (reduced)** — voice-mode select (8T/4V/2V/1V) + **scale *selection*** from
    TT's scale system. **Per-note scale editing dropped** (Ansible's `scale_data[scale][row]`
    edit at `:4178`) — excluded by A4 (reuse TT scales, no per-scene `scale_data`).
  - **Headless (no grid)** — engine runs on its own clock; fully drivable from keyboard + ops.
    Cost is only ensuring `mp_grid_process_key()`/`mp_grid_refresh()` no-op cleanly with no grid.

### B. Missing content to add (no decision needed)

- **B1 — Per-scene init/default path.** Add `ss_mp_init()` (analogous to `ss_grid_init`) +
  firstrun defaults for a fresh scene.
- **B2 — Global-namespace collision (porting hazard).** Ansible MP uses bare globals (`mode`,
  `state[8]`, `position[8]`, `tick[8]`, `reset[8]`, `edit_row`, … `ansible_grid.c:3519-3535`)
  that will collide in TT — must be namespaced (struct or `mp_` prefix) during extraction.
- **B3 — Help + docs.** New mode needs `help_mode.c` text + docs-pipeline entries.
- **B4 — Firmware-build verification gate.** Host unit tests won't catch firmware-only
  breakage; add an explicit AVR32/Docker build step (`dewb/monome-build`, `--platform
  linux/amd64`).
- **B5 — Grid varibright/mono fallback** in `mp_grid_refresh()`, as TT grid-control already does.

### C. Minor
- Arc explicitly out of scope.
- Serialization version bump — already in risks.
- CV resting state on exit — already flagged.

---

## Planned feature: grid scale editing (Ansible-style)

Restore the per-note scale editing that A4 dropped. Today the scale is chosen
with `[`/`]` from 7 fixed diatonic modes + chromatic (derived from `SCALE_INT`);
you cannot edit the intervals. Ansible let you both **select** a scale (16 slots)
and **draw its intervals** on the grid's config view. This plans that.

### What Ansible did (reference)
Config view (front-button): voice mode (top-left), **scale select** = 2 rows x 8
= 16 slots (`m.scale = (y-6)*8 + x`), **per-degree interval editor** on the right
half (`scale_data[m.scale][7-y] = x-8`), then `calc_scale`. Scales lived in a
**global** `f.scale[16][8]` (shared across presets), 7 diatonic + 9 blank.

### Decisions needed (recommendations first)

- **D1 — Storage: global bank vs per-scene.** *Recommend a global editable bank*
  `scale_bank[16][8]` in `nvram_data_t` (like `cal`/`device_config`), matching
  Ansible. Cost: 128 B once (not x30 scenes) — trivial against the ~5.4 KB free
  in the NVRAM region. `mp_config_t.scale` (per-scene) still selects the slot.
  Alternative: one editable scale per scene in `mp_config_t` (+8 B/scene = 240 B)
  if scales should be scene-specific — but you lose the shared library. **Global.**
- **D2 — Bank size:** *16 x 8* (Ansible parity, fits a 2x8 select grid, 128 B) vs
  8 x 8 (compact). **Recommend 16.**
- **D3 — Grid surface trigger:** *tie it to the OLED Config view* (`3`): pressing
  `3` switches the grid to the scale editor (voice/scale). Unifies the keyboard
  view with the grid, avoids a 4th col-hold sub-mode. **Recommend view-driven.**
- **D4 — Layout on 16x8:** scale-slot select (2 rows x 8 = 16), per-degree
  interval editor (8 rows x interval columns 0-N). Exact cell map to design in
  Phase 2; port Ansible's where it maps cleanly.
- **D5 — `[`/`]` semantics:** repurpose to step the **bank slot** (0-15) instead
  of the fixed modes; grid edits the selected slot's intervals. **Recommend.**
- **D6 — 8T:** scale is irrelevant (gates); Config view hides the scale editor in
  8T (as Ansible did). Keep.

### Wiring
- `mp_apply_scale()` reads the selected `scale_bank[cfg.scale]` intervals instead
  of `SCALE_INT`/chromatic; call it on edit + slot change + mode enter.
- Bank init (firstrun): rows 0-6 = the 7 diatonic modes (from `SCALE_INT`), row 7
  = chromatic, 8-15 = a sensible default (chromatic or copies) — editable.

### Footprint
- **NVRAM:** +128 B in `nvram_data_t` (content 189,064 -> 189,192 of 190 KB;
  ~5.4 KB slack remains). Changes `nvram_data_t` layout -> **`FIRSTRUN_KEY`
  0x23 -> 0x24** (another flash reinit; existing scenes wiped — acceptable).
- **Code:** grid editor + select logic ~1-2 KB `.text`. Only **~4 KB program-flash
  headroom** remains — this is the binding constraint. If it doesn't fit, reclaim
  via the `tele_data_t` 8->4 B change (frees ~10 KB) before landing this.

### Phases
1. **Storage + wiring — ✅ DONE (builds clean, exit 0).** `scale_bank[16][8]` added to
   `nvram_data_t` (`flash.h`, `MP_SCALE_SLOTS 16`); `flash.c` inits it in firstrun (0-6 diatonic
   via `SCALE_INT`, 7-15 chromatic) + `flash_get/update_scale_bank` accessors; `FIRSTRUN_KEY`
   0x23→0x24. Mode keeps a RAM mirror loaded on init; `mp_apply_scale()` now reads the bank slot
   (no more `SCALE_INT` derivation); `[`/`]` cycle all 16 slots; Config view shows slot # + name
   (0-7) / "USER" (8-15). `.flash_nvram` 189,064 → **189,192 (+128 B, ~5.4 KB slack)**; `.text`
   +124 B. No editor yet (Phase 2) — slots 8-15 are chromatic until edited.
2. **Grid editor — ✅ DONE (builds clean, exit 0).** `mp_grid_scale_key` /
   `mp_grid_scale_refresh` in `meadowphysics_grid.c` (host-tested): rows 6-7 x cols 0-7 select
   the slot, the right half (cols 8-15, one row per degree, row y = degree 7-y) sets each degree's
   interval — ported from Ansible's config view. Shown only while **actively viewing the Config
   view** (`active && view==CONFIG`); background-running keeps the positions animation. Edits write
   the RAM bank + `flash_update_scale_bank` (write-through) and re-apply live. `.text` +404 B
   (~3.7 KB headroom left — the `tele_data_t` reclaim was **not** needed).
3. **Keyboard/OLED:** `[`/`]` -> bank slot; Config view shows slot + intervals.
4. **Docs/help.**

### Risks
- **Program-flash budget (~4 KB)** is the real ceiling — the grid editor may not
  fit without the `tele_data_t` reclaim. Measure early (Phase 1) and decide.
- Another `FIRSTRUN_KEY` bump wipes user scenes on upgrade.
- Grid layout/interaction complexity on 16x8; needs bench validation.
- Making scales global changes the mental model (edits affect all scenes) — the
  A4 intent was to *reuse* TT scales; confirm a private MP bank is wanted vs
  hooking TT's own scale system.
