# Plan: Native Kria mode in Teletype ("live-together")

Implementation plan for porting Ansible's **Kria** sequencer into the modded
Teletype firmware as a native mode, running **alongside** the Teletype scripting
engine (shared hardware, scriptable via ops), mirroring the proven
`meadowphysics-mode` architecture.

- Source app: `~/Claude/ansible` — Kria lives in `src/ansible_grid.c` (~211 refs)
  and `src/ansible_grid.h` (structs `kria_track` / `kria_pattern` / `kria_data_t`
  / `kria_state_t`).
- Target: the modded Teletype (`~/repo/teletype.git`, branches `main` →
  `usb-hub-support` → `meadowphysics-mode`). Base the Kria branch on
  `meadowphysics-mode` so it inherits the hub + the MP mode + the shared-output
  plumbing.
- Precedent: `MEADOWPHYSICS_PORT_PLAN.md` on the `meadowphysics-mode` branch.
  This plan follows the same three-tier structure and reuses its seams verbatim
  where possible.

---

## 0. The flash reality — READ THIS FIRST

The AT32UC3B0512 has **512 KB of on-chip flash**, shared by program code and the
scene NVRAM. There is no external storage. Measured from the current
`meadowphysics-mode` build's `teletype.map`:

| Region | Address span | Size |
|--------|-------------|------|
| Program code + rodata + data image | `0x80000000` → `0x8004fd50` | **~319 KB** |
| **Free gap** | `0x8004fd50` → `0x80050800` | **~2.7 KB** |
| Scene NVRAM (`.flash_nvram`, `__flash_nvram_size__`) | `0x80050800` → `0x80080000` | **190 KB** |

(The factory DFU/ISP bootloader lives in a separate protected ROM and does **not**
consume this 512 KB — the whole image is code + 2.7 KB slack + NVRAM.)

**We have ~2.7 KB of headroom and Kria needs far more than that.** Every design
decision below is driven by this. RAM is *not* the constraint (~51 KB SRAM free;
a Kria working set fits fine) — **flash is**.

### What Kria costs

Kria's data model is large. From `ansible_grid.h`:

```
kria_track   ≈ 300 B   (8 step-arrays[16] + p[7][16] + loop/tmul params)
kria_pattern = 4 × kria_track + scale        ≈ 1.2 KB
kria_data_t  = 16 × kria_pattern + meta[128] + glyph ≈ 19 KB   ← one "preset"
```

Ansible stores **8** of these (`kria_data_t k[GRID_PRESETS]` ≈ **152 KB**). We
cannot afford that. Plus the code: MP added ~22 KB of program flash; Kria's grid
UI alone (9 view pages, meta-sequencing, mod modes) is 2–3× MP, so budget
**~35–50 KB of code**.

### The budget equation

```
Need ≈ Kria code (35–50 KB) + Kria NVRAM data (see options) + margin
Have ≈ 2.7 KB  →  must reclaim ~50–70 KB
```

Two independent levers, use both:

**Lever A — shrink Kria's own data footprint** (biggest, cheapest win):
- Store **one** global Kria state, not 8 presets: **19 KB → save ~133 KB** vs Ansible.
- Optionally cut `KRIA_NUM_PATTERNS` 16 → 8 or 4: `kria_data_t` ~19 KB →
  ~10 KB / ~5 KB. Trade: fewer pattern slots per Kria "song".

**Lever B — reclaim NVRAM by dropping Teletype scenes** (direct UX cost):
- Each scene slot ≈ **6.3 KB** (190 KB / 30). Dropping N scenes frees N × 6.3 KB
  of NVRAM, then lower `__flash_nvram_size__` by the same and hand it to code.
- e.g. 30 → 24 scenes frees ~38 KB; 30 → 20 frees ~63 KB.

### Three concrete budget scenarios

| Scenario | KRIA_NUM_PATTERNS | Kria presets | Scenes | Kria NVRAM | Frees from scenes | Net for code | Feasible? |
|----------|------------------|-------------|--------|-----------|------------------|-------------|-----------|
| **A. Lean** (recommended) | 8 | 1 global | 24 | ~10 KB | ~38 KB | ~28 KB code + margin | ✅ comfortable |
| **B. Full Kria** | 16 | 1 global | 20 | ~19 KB | ~63 KB | ~44 KB code | ✅ tight |
| **C. Multi-preset** | 16 | 4 global | 16 | ~76 KB | ~88 KB | ~12 KB code | ❌ code won't fit |

> **DECISION NEEDED (§3, item P):** pick the pattern-count / preset / scene
> tradeoff. This plan assumes **Scenario A** unless you choose otherwise. Nail
> this *before* writing the flash layout — it's the hardest thing to change later.

> **Ground truth beats estimates.** Before committing, build a Kria *stub* (empty
> mode compiled in, `kria_data_t` sized as chosen, linked into NVRAM) and read
> `teletype.map`: confirm `code_end < nvram_start`. That one build validates the
> whole budget. My code estimates are from line/struct inspection, not a link.

### Phase 0 result — VERIFIED ✅ (Scenario B chosen, GO)

Ran the spike on branch `kria` (off `meadowphysics-mode`) in `~/Claude/teletype`,
built with `dewb/monome-build`. **Chose Scenario B** (full 16-pattern Kria).
Edits: added `src/kria_engine.h` (structs at `KRIA_NUM_PATTERNS = 16`), added
`kria_config_t kria` to `nvram_data_t`, `SCENE_SLOTS 30 → 20`,
`__flash_nvram_size__ 190K → 145K`. Measured from `teletype.map`:

| Quantity | Scenario B (chosen) | Scenario A (for reference) |
|----------|--------------------|----------------------------|
| Kria patterns / TT scenes | 16 / 20 | 8 / 24 |
| `sizeof(nvram_data_t)` | **144,796 B** (145K region, ~3.7 KB margin) | 160,772 B (160K region) |
| Code end (`_data_lma` end) | `0x8004fd4c` (layout-only) | `0x8004fd4c` |
| NVRAM region start | `0x8005bc00` | `0x80058000` |
| **Free program flash for Kria code** | **48,820 B ≈ 47.7 KB** | 33,460 B ≈ 32.7 KB |
| Kria code estimate (MP was ~9 KB; Kria ≈ 2–3×) | 20–27 KB → **FITS, ~21–28 KB margin** | FITS, ~6–13 KB |

Note: B frees *more* code space than A because dropping 10 scenes (~63 KB)
outweighs the extra 8 Kria patterns (~9 KB) — it costs only 4 more scenes than A.

Baseline calibration: the MP mode added only **~9 KB of text** over the hub
branch (`0x4a352 → 0x4cb5a`), which anchors the Kria estimate. RAM is not a
constraint: `_end = 0xae50`, heap to `0x16000` → **~45 KB free** for the live
`kria_config_t` working copy (~18.5 KB) plus runtime state.

**Verdict: Scenario B is viable with comfortable margin — proceed to Phase 1.**

---

## 1. Goal & decisions

| # | Decision | Resolution |
|---|----------|-----------|
| — | Integration approach | **Native port** — new `M_KRIA` mode running the Kria engine inside TT, driving TT's own 4 CV + 4 TR. Live-together with the scripting engine (not i2c to an external Ansible). |
| 1 | Output ownership | **Shared, arbitrated** — reuse MP's `suppresses_output()` + `writing` gate in `tele_tr`/`tele_cv`. Kria owns the channels it's actively driving; scripts drive the rest. |
| 2 | Persistence | **Global bank** (not per-scene) — one `kria_data_t` at the `nvram_data_t` top level (like MP's `scale_bank`), so cost is fixed and does **not** multiply by scene count. |
| 3 | Clock | **Kria-specific** — Kria is divider-heavy (per-track `tmul`, per-param time-mod, meta-sequencing). Needs a richer clock than MP's 2-fire toggle (see §7). External clock optional on a Tr input. |
| 4 | Grid | **16×8** primary (Kria's native layout); degrade on 8×8. |
| 5 | Input model | Grid = editing surface; **USB keyboard** = view/transport/config; **TT ops** = scriptable control. All concurrent (hub already supports this). |
| P | **Pattern/preset/scene budget** | **OPEN — see §0 scenarios. Default: A (8 patterns, 1 preset, 24 scenes).** |

---

## 2. What Kria is (for the engine port)

Four independent tracks. Each track has **7 parameter loops** (`KRIA_NUM_PARAMS`):
trigger, note, octave, duration, repeat, alt-note, glide — each an independent
16-step loop with its own start/end/length (`lstart/lend/llen`), swap, and time
multiplier (`tmul`). Per track: direction (fwd/rev/tri/drunk/random), duration
multiplier, octave shift. A **pattern** = 4 tracks + a scale; a Kria "song" holds
up to 16 patterns plus a **meta-sequencer** (`meta_pat[64]`/`meta_steps[64]`) that
chains patterns. Grid UI = 9 pages (`kria_modes_t`: Tr/Note/Oct/Dur/Rpt/AltNote/
Glide/Scale/Pattern) × mod modes (loop/time/prob).

Port target in Ansible: `handler_Kria*` (key/tr/grid/refresh), `clock_kria_track`,
`clock_kria_note`, `kria_next_step`, `kria_set_note`, `default_kria` in
`ansible_grid.c`.

---

## 3. Architecture — mirror the MP three-tier split

```
        TT scripts / KR.* ops              keyboard / grid / OLED
               │                                  │
   src/ops/kria.c  ── teletype_io.h seam ──► module/kria_mode.c   ← HOST / MODE SHELL
                                                  │   (instances, timer, ownership,
                          ┌───────────────────────┼───── flash load/save, views, grid)
                          │                        │                        │
              src/kria_clock.c        src/kria_engine.c          src/kria_binding.c   ← PURE / PORTABLE
              (tempo, per-track       (tracks, steps, loops,     (scale degree → CV,
               dividers, meta)         meta-seq, scale)           tele_tr/tele_cv)
                                              │                        │
                                              └──── kria_output_t vtable ┘
                                                                       │
                                        module/main.c: tele_tr() / tele_cv()  ← HARDWARE
```

Two seams to copy **exactly** from MP (they're what make it clean):
1. **`kria_output_t` vtable** — engine emits via injected function pointers
   (`tr`, `cv`, `cv_gate`, `ctx`); keeps `src/kria_engine.c` hardware-free and
   host-unit-testable (add `tests/kria_tests.c`). Only `kria_binding.c` includes
   `teletype_io.h`/`music.h`.
2. **`writing` + `suppresses_output()` ownership gate** inside `tele_tr`/`tele_cv`
   — lets Kria and scripts share the 4 jacks.

### New files
```
src/kria_engine.{h,c}     kria_engine_t{cfg, rt, out, rnd}; step/clock/reset/mute API
src/kria_clock.{h,c}      per-track divider + meta clock (richer than MP)
src/kria_binding.{h,c}    kria_output_t → tele_tr/tele_cv (+ note_to_cv, reuse MP's)
src/kria_grid.{h,c}       grid render + key handling for the 9 pages
module/kria_mode.{h,c}    host shell (see §5 seams)
src/ops/kria.{h,c}        native KR.* ops (see §6)
tests/kria_tests.c        host tests for the pure engine
```

### Engine structs (adapt from `ansible_grid.h`)
- `kria_config_t` (persistent, serialized): the `kria_data_t` payload (tracks ×
  params × steps, scale, meta) at the chosen `KRIA_NUM_PATTERNS`. Add a
  `kria_engine_config_valid()` that bounds-checks **every** index field
  (lstart/lend/llen ≤ 15, scale ≤ max, direction ≤ 4, pattern ≤ N) — stale flash
  must be rejected, exactly like `mp_engine_config_valid`.
- `kria_runtime_t` (ephemeral, never serialized): `pos[4][7]`, `pos_mul[4][7]`,
  per-track note/gate timers, mutes, blink flags, meta position.
- `kria_output_t`: `{tr, cv, cv_gate, ctx}` (same shape as `mp_output_t`).

---

## 4. Voice / output mapping

Kria is natively **4 tracks → 4 voices**. Clean 1:1 map to TT's 4 CV + 4 TR:
- Track *n* → `CV n` (pitch = note+oct+transpose, via `kria_note_to_cv`, reuse
  MP's `mp_note_to_cv`) + `TR n` (gate; duration/repeat/glide shape it).
- `kria_owned_channels()` = number of un-muted tracks; muted tracks release their
  jack back to scripts (via the `suppresses_output` gate).

---

## 5. Module integration seams (from the MP map — 7 files)

The `meadowphysics-mode` branch defines the exact recipe. Kria repeats it with
`kria`/`M_KRIA`/`G_KRIA` names.

**`module/kria_mode.h`** — public contract:
```c
void set_kria_mode(void);
void kria_mode_exit(void);
void process_kria_keys(uint8_t key, uint8_t mod_key, bool is_held_key);
uint8_t screen_refresh_kria(void);
bool kria_owns_grid(void);
void kria_grid_key(uint8_t x, uint8_t y, uint8_t z);
void kria_grid_render(void);
void kria_toggle_run(void);
void kria_clock_tick(void);
bool kria_external_clock(uint8_t level);
bool kria_suppresses_output(uint8_t ch);
// ops seam impls
void kria_op_reset(int16_t track);
// ... (see §6)
```
File-static state block like MP: `initialized / active / kria_running / writing /
timer_enabled / dirty / view`. Two independent booleans — **`active`** (front
view: keyboard+screen) and **`kria_running`** (background engine: timer + owns
jacks); `kria_owns_grid() = active || kria_running`.

**`module/main.c`** — 8 edits:
1. `#include "kria_mode.h"`.
2. `set_mode()`: `case M_KRIA: set_kria_mode();` + on leaving, `kria_mode_exit()`.
3. `handler_ScreenRefresh()`: `case M_KRIA: screen_dirty = screen_refresh_kria();`.
4. `process_keypress()`: `case M_KRIA: process_kria_keys(...)`.
5. `process_global_keys()`: `alt-K` toggle mode + a play/pause hotkey.
6. `handler_AppCustom(data)`: **use a new selector** `data == 2` for Kria's clock
   tick (MP owns `data == 1`) → `kria_clock_tick()`.
7. `handler_Trigger()`: route `KR_EXT_CLOCK_INPUT` → `kria_external_clock(level)`.
8. `tele_tr()` / `tele_cv()`: add `if (kria_suppresses_output(i)) return;`
   alongside MP's guard.

**`module/grid.c`** — 5 edits: `#include`; add `G_KRIA` to `grid_control_mode_t`;
map it in `grid_set_control_mode`; press branch at top of `grid_process_key`
(`if (kria_owns_grid()) { kria_grid_key(x,y,z); ... return; }`); render branch at
top of `grid_refresh`.

**`module/globals.h`** — add `M_KRIA` to `tele_mode_t` (append at end to avoid
renumbering MP/preset modes).

**`module/help_mode.c`** — one new help page: bump `HELP_PAGES`, retitle the
`"N/NN"` strings, add `helpNN`/`HELPNN_LENGTH`, extend `help_pages[]`/`help_length[]`.

**`module/config.mk`** — add `../module/kria_mode.c` and `../src/kria_*.c` to
`CSRCS`; **update `__flash_nvram_size__`** and `SCENE_SLOTS` (flash.h) per the
chosen §0 scenario (e.g. 190K → smaller, 30 → 24).

**`src/state.h` / `src/state.c`** — **NOTE the divergence from MP:** MP put its
config *per-scene* (`mp_config_t mp` in `scene_state_t`). Kria uses a **global
bank**, so it does *not* go in `scene_state_t`. Instead the live `kria_config_t`
lives in the mode shell and is loaded/stored via the flash bank API (§6). You may
still add a tiny per-scene `uint8_t kria_pattern`/enable byte if you want scene
recall to *select* a Kria pattern, but the heavy data stays global.

---

## 6. Persistence — global bank (mirror MP's `scale_bank`)

MP demonstrated the exact pattern: a fixed top-level array in `nvram_data_t`,
outside `scenes[]`, with whole-blob read/write. Do the same for Kria.

**`module/flash.h`**:
```c
typedef struct { /* the chosen kria_data_t payload */ } kria_config_t;

typedef struct {
    nvram_scene_t scenes[SCENE_SLOTS];   // SCENE_SLOTS reduced per §0
    uint8_t  last_scene;
    tele_mode_t last_mode;
    uint8_t  fresh;
    cal_data_t cal;
    device_config_t device_config;
    uint8_t  scale_bank[MP_SCALE_SLOTS][8];  // existing MP global bank
    kria_config_t kria;                       // NEW: single global Kria state
} nvram_data_t;

void flash_get_kria(kria_config_t* dst);
void flash_update_kria(const kria_config_t* src);
```

**`module/flash.c`** (copy the `scale_bank` accessors):
```c
void flash_get_kria(kria_config_t* dst)    { memcpy(dst, &f.kria, sizeof(f.kria)); }
void flash_update_kria(const kria_config_t* s){ flashc_memcpy((void*)&f.kria, s, sizeof(f.kria), true); }
```
In `flash_prepare()` (first-run), stage a `kria_config_t` default in RAM via
`kria_engine_set_defaults()` then one `flashc_memcpy` into `f.kria`.

**Migration:** there is no field-level migration — **bump `FIRSTRUN_KEY`**
(currently `0x24` → `0x25`). This wipes NVRAM on upgrade (documented MP behavior).
Warn users to back up scenes before flashing.

**Budget check (do this in code):** after defining `kria_config_t`, ensure
`sizeof(nvram_data_t) <= __flash_nvram_size__`. Consider adding a compile-time
`_Static_assert` (or a link-time check) — the MP branch relies on a *manual*
budget with no assert; add one so an oversize struct fails the build instead of
silently colliding with code.

---

## 7. Clock (Kria is harder than MP)

MP's clock is a 2-fire phase toggle. Kria needs:
- A base tick, then **per-track, per-param time division** (`tmul[track][param]`)
  — each parameter loop advances at its own rate.
- **Meta-sequencing** — pattern changes chained by `meta_pat`/`meta_steps`.
- Per-track `dur_mul` and duration/repeat/glide note-shaping timers.

Design `src/kria_clock.c` to own the base period + a divider counter per
(track,param), and to signal the mode shell which tracks/params step on each tick.
Keep it pure (no engine pointer), like MP: the shell calls `kria_engine_clock()`
with the set of advancing lanes. External clock: one Tr edge = one base tick.
Reuse MP's ISR→`kEventAppCustom` bridge (with `data == 2`), wrapping the engine
call in `writing = true/false`.

---

## 8. Ops (`KR.*`)

MP repurposed 3 existing i2c op names to the native engine. Kria's stock i2c ops
in `src/ops/kria.c` (if present on `main`) can be retargeted the same way;
genuinely new ops need the full registration path.

Recommended minimal native op set (headless/scriptable control): `KR.MUTE t x`,
`KR.PAT p` (select pattern), `KR.RESET t`, `KR.PERIOD n` (tempo), `KR.TRANS t x`
(transpose). Each op:
1. enum in `src/ops/op_enum.h`;
2. pointer in the `src/ops/op.c` table;
3. tokenizer rule in `src/match_token.rl` (regenerate `match_token.c` with ragel);
4. `extern const tele_op_t op_KR_*` in `src/ops/kria.h`;
5. `MAKE_GET_OP`/`MAKE_GET_SET_OP` body in `src/ops/kria.c` that calls a
   `kria_op_*` seam;
6. seam declared in `src/teletype_io.h`;
7. **two** seam impls: firmware in `module/kria_mode.c` (reaches the `kria_eng`
   singleton via `kria_engine_*`) **and** a stub in `simulator/tt.c` (else the
   test/sim build won't link).

---

## 9. Phasing (each phase compiles + is testable)

- **Phase 0 — Budget spike (do first, ~½ day).** Define `kria_config_t` at the
  chosen size, add the flash bank + `SCENE_SLOTS`/`__flash_nvram_size__` change,
  compile an empty `M_KRIA` stub. Read `teletype.map`: confirm code end < NVRAM
  start with margin. **Go/no-go gate for the whole feature.**
- **Phase 1 — Pure engine + tests. ✅ DONE.** Ported the Kria sequencer into
  `src/kria_engine.{h,c}` behind the `kria_output_t` vtable (tr / cv-as-semitones
  / cv_slew), injected RNG + caller-owned scale bank. Engine/shell split: the
  engine makes all sequencing decisions (position advance, per-param dividers,
  latching, trigger fire, pitch, meta-chaining, scale) and exposes
  `kria_engine_note_off()` / `kria_engine_repeat()` for the shell's timers to call;
  real-tick gate/repeat scaling is deferred to the shell (no `get_ticks`/timers in
  the engine). `tests/kria_tests.c` (13 tests) covers defaults/config-validation,
  cumulative scale, forward/reverse/random stepping, tmul dividers, loop bounds,
  mute, note→scale→octave pitch, meta pattern-chaining, rising-edge-only, and
  repeats. **Full suite green: 98/98 (13 Kria + 25 MP, no regressions).** Compiles
  clean under `-Wall -std=c99`. Engine not yet in the firmware build (`config.mk`)
  — it gets wired in Phase 2 via the binding.
- **Phase 2 — Binding + clock. ✅ DONE (compile-on-target + tested).**
  `src/kria_binding.{c,h}`: `kria_output_t` vtable → `tele_tr` (gates),
  `tele_cv` + `kria_note_to_cv` (ET, same tuning as N op / MP) (pitch),
  `tele_cv_slew` (glide). `src/kria_clock.{c,h}`: internal/external phase
  arbitration (2-fire toggle, rising-edge steps) + the deferred real-tick
  scaling — `kria_clock_scale_duration` (`unscaled*clock_delta*tmul/384`) and
  `kria_clock_repeat_ticks` (`clock_delta*tmul/rpt`), Ansible-faithful. All three
  `kria_*.c` added to `module/config.mk`; they compile under avr32-gcc and the
  firmware links (dead-code-eliminated until the shell calls them → image
  unchanged, 47.7 KB budget intact). Tests +6 (note→ET, vtable, clock toggle/
  external/clamp/scaling); **104/104 green**. Deferred to Phase 4 (needs the
  shell's `kria_running`/`writing` state): the `suppresses_output`/`writing`
  output-ownership gate in `main.c` `tele_tr`/`tele_cv`.
- **Phase 3 — Grid UI. ✅ DONE (on-target compile + smoke-tested).**
  `src/kria_grid.{c,h}`: all 9 pages (tr/note/altnote/oct/dur/rpt/glide/scale/
  pattern) + mod overlays (loop/time/prob) + bottom control row, rendering into
  a 16×8 LED buffer and handling keys off `kria_engine_t` + `kria_grid_state_t`.
  Faithful loop-range math (lswap wrap, `loop_sync` fan-out), `tmul` fan-out, the
  two-press loop gesture w/ note↔tr coupling; `edit_pattern = meta_lock ? stored
  : cfg.pattern`. `kria_grid_key_hold` for long-press. Added to `config.mk`
  (compiles avr32, firmware links, still dead-code-eliminated → 47.7 KB budget
  intact). Tests +5 (defaults, mode/track select, tr toggle, loop gesture, render
  smoke); **109/109 green**. Deviations (need hardware to tune, documented
  in-file): primary 16×8 view only; pattern-change on press; mRpt-vrange &
  meta-slot loop gestures are TODO fallbacks. **Visual/interaction fidelity
  unverified — needs a grid.** Shell wiring (`kria_owns_grid`/`grid_key`/
  `grid_render`) is Phase 4.
- **Phase 4 — Mode shell + firmware integration. ✅ DONE (compile-clean,
  M_KRIA reachable; needs hardware validation).** `module/kria_mode.{c,h}`:
  file-static engine/clock/grid, clock softTimer→`kEventAppCustom data==2`,
  per-track note-off/repeat/blink timers (ISR callbacks post events 10-13/20-23,
  handled in the event loop — no ISR output), a custom output vtable that wraps
  `tele_tr`/`tele_cv`(ET)/`tele_cv_slew` and schedules real-tick gate/repeat
  timers on note-on. `writing`/`kria_suppresses_output` per-track gate (muted
  tracks free for scripts). Grid ownership `active||running`. Keyboard (Space/R/
  X/-/=/S), compact OLED, shares MP scale bank. Wired into `globals.h` (M_KRIA),
  `main.c` (all seams incl. alt-K, `handler_AppCustom`, ext-clock input 1,
  `tele_tr`/`tele_cv` gate), `grid.c` (G_KRIA + ownership routing),
  `flash.{c,h}` (`flash_get/update_kria`, `FIRSTRUN_KEY` 0x24→0x25),
  `config.mk`. **Kria code now reachable: firmware +13.6 KB (text 0x4cb5a→
  0x500da), ~34.4 KB headroom left; tests 109/109 green.** Deferred: help page,
  OLED views (grid is primary UI), and op retargeting (existing `KR.*` i2c ops
  left intact). **Flashed & hardware-validated 2026-07-04: boots clean, Kria
  mode reachable via alt-K and plays (initial smoke test passed).**
- **Phase 5 — Persistence.** `flash_get/update_kria`, first-run defaults,
  `FIRSTRUN_KEY` bump, `kria_engine_config_valid` on load.
- **Phase 6 — Ops.** The `KR.*` set + simulator stubs.
- **Phase 7 — Hardware soak.** Clock stability, voice allocation, jack sharing
  with scripts, hub + grid + keyboard concurrent.

---

## 9b. Shipped beyond the core port (all on hardware)

After Phases 0–4, these were added and flashed:

- **Native `KR.*` / `MP.*` ops** — the full `KR.*` family retargeted from
  external-Ansible i2c to the on-board engine, plus `KR.RUN` / `MP.RUN`. Docs
  updated (`docs/ops/ansible.toml`, `meadowphysics.toml`). Commit `6527518` /
  `e78de0c`.
- **Time + Config grid views** (Ansible Key 1 / Key 2) — keyboard `2` / `3`:
  rough/fine tempo, and note-sync / loop-sync / note-tie / meta-reset / div-sync.
  Commit `cf57539`.
- **i2c follower output** — all six followers (JF, TXo, ER-301, Disting EX, W/syn,
  Crow) + both Ansible ii pages (toggle + per-follower config), a port of
  `ansible_ii_leader.c`. Keyboard `4`. Driven by **both Kria and Meadowphysics**
  (shared global follower table). See **`KRIA_I2C_PLAN.md`**. Commits
  `c41962d` → `98cdce4`, `b7a9d3f`. *(ER-301/Disting/Crow/WSYN hardware-unverified.)*

**Kria keyboard map:** `alt-K` enter · `1/2/3/4` = seq / time / config / i2c
views · `Space` run · `R` reset · `X` ext-clock · `−/=` tempo · `S` save.

- **USB MIDI-out `MO.*` ops** — merged from the `midi-out` branch. Merge commit
  `6bd4d24` (backup branch `kria-backup`); help pages → 19 (MEADOWPHYSICS 18,
  MIDI OUT 19). Tests 111/111.
- **Preset-knob scene bound** — the param knob mapped its full travel to a
  hardcoded 32-scene sweep (`adc[1]>>7` / `>>6`), leftover from `SCENE_SLOTS==32`;
  with Scenario B's `SCENE_SLOTS==20` the top third rolled into non-existent
  scenes 20–31. Rescaled all three sites against `SCENE_SLOTS` in `module/main.c`.
  Commit `bbbf401`.

Still open: a scripting op for i2c follower routing; hardware verification of the
four untested followers (ER-301 / Disting / Crow / W/syn).

---

## 10. Risks & watch-items

- **Flash ceiling is the whole game.** ~2.7 KB slack today. If the Phase 0 spike
  shows the chosen scenario doesn't fit, shrink Kria data (fewer patterns) before
  cutting more scenes. Add the `sizeof(nvram_data_t)` assert.
- **RAM working set:** one live `kria_data_t` (~5–19 KB) sits in SRAM (~51 KB
  free) — fine, but confirm after adding the engine's runtime arrays.
- **Clock complexity** is the main *code* risk (per-lane dividers + meta); it's
  where Ansible's Kria is most intricate. Port carefully with tests.
- **Two apps, one `handler_AppCustom` channel:** MP uses `data == 1`; Kria must
  use a distinct selector (`data == 2`). Don't collide.
- **Playing both MP and Kria at once** is possible in principle (both are
  background engines with output gates) but they'd fight for the 4 jacks —
  decide whether starting one stops the other.
- **NVRAM wipe on upgrade** (FIRSTRUN_KEY bump). Communicate the scene-count
  reduction and backup need to users.
- **`SCENE_SLOTS` fan-out:** reducing scene count means any code that maps a
  full-range control onto scene indices must be rescaled, not just the array
  bounds. The param-knob sweep (`main.c` `handler_PollADC` / front-panel entry)
  was hardcoded for 32 and missed the first two cuts — now derives from
  `SCENE_SLOTS`. Audit for other hardcoded `>>7` / `>>6` if the count changes.
- **Simulator parity:** every new `teletype_io.h` seam needs a `simulator/tt.c`
  stub or the test build breaks.
```
