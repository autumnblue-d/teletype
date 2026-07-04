# Plan: Native Earthsea mode in Teletype

Implementation plan for porting Ansible's **Earthsea** (grid app, Ansible ≥ 3.0)
into the modded Teletype firmware as a native mode, running alongside the
scripting engine — same live-together architecture as the shipped
Meadowphysics and Kria ports.

- Source app: `~/Claude/ansible` (v3.2.0). Earthsea lives in
  `src/ansible_grid.c` **lines ~4625–5795 (~1,170 lines)** and
  `src/ansible_grid.h` (structs `es_note_t` / `es_event_t` / `es_pattern_t` /
  `es_data_t`, lines 154–213).
- Target: this repo, branch `kria` (or a new `earthsea` branch off it) — it
  already carries MP + Kria + the shared i2c follower layer + MO/I2M MIDI out.
- Precedent: `KRIA_PORT_PLAN.md` / `MEADOWPHYSICS_PORT_PLAN.md`. This plan
  reuses their three-tier split and every integration seam verbatim where
  possible. Earthsea is the **smallest** of the three ports: ~1.2 K source
  lines vs Kria's several thousand, and no clock-divider machinery.

---

## 0. The flash reality — measured from the current build

From the current `module/teletype.map` (post MP + Kria + i2c followers + MO/I2M):

| Quantity | Value |
|----------|-------|
| Code + data image end (LMA of `.flash_nvram`) | `0x80056610` |
| NVRAM region base (`__flash_nvram_size__ = 145K`) | `0x8005bc00` |
| **Free program flash for ES code** | `0x55f0` = **~21.5 KB** |
| `sizeof(nvram_data_t)` (`.flash_nvram` size `0x23654`) | 144,980 B |
| **Free NVRAM margin** (145 K region = 148,480 B) | **~3.5 KB** |

### What Earthsea costs

Data (from `ansible_grid.h`, `ES_EVENTS_PER_PATTERN = 128`):

```
es_event_t   = 4 B    (on, index, u16 interval)
es_pattern_t = 528 B  (128 events + loop/root/edge/voices/dir/linearize params)
es_data_t    ≈ 8.7 KB (16 patterns + keymap[256] + arp/voices/octave/scale)
```

One **global** bank (dropping Ansible's 8 presets + glyphs, exactly like the
Kria decision) is ~8.7 KB — that does **not** fit the current 3.5 KB NVRAM
margin, so scenes must give a little.

Code: Ansible's ES is ~1,170 lines with no per-lane clock system. MP cost
~9 KB of text; Kria (engine+grid+shell) ~13.6 KB. Estimate **9–13 KB** for ES
→ fits the 21.5 KB code headroom with margin, **no `__flash_nvram_size__`
change needed**.

### Budget scenarios

Scene slot ≈ 6.3 KB (verify with a `sizeof` probe before committing).

| Scenario | ES patterns | ES bank | Scenes | NVRAM margin left | Verdict |
|----------|------------|---------|--------|-------------------|---------|
| **A (recommended)** | 16 (full) | ~8.7 KB | 20 → **18** | ~7.4 KB | ✅ comfortable |
| B | 8 | ~4.5 KB | 20 → 19 | ~5.4 KB | ✅ but half the pattern slots |
| C | 4 | ~2.4 KB | 20 (unchanged) | ~1.1 KB | ⚠️ no scene cost, no margin |

RAM: the live `es_config_t` working copy (~8.7 KB) sits in SRAM next to
Kria's (~18.5 KB). After Kria, roughly ~26 KB was free — ES leaves ~17 KB.
Fine, but re-check `_end` in the map after Phase 4.

> **Phase 0 gate (same as Kria):** define `es_config_t` at the chosen size,
> add it to `nvram_data_t`, drop `SCENE_SLOTS` per the scenario, compile an
> empty `M_EARTHSEA` stub, and read `teletype.map`. Confirm image end <
> `0x8005bc00` and `sizeof(nvram_data_t)` ≤ 148,480 before writing any engine
> code.

### Phase 0 result — VERIFIED ✅ (Scenario A, GO)

Ran the spike on branch `kria`: added `src/es_engine.h` (full engine contract;
`es_config_t` measured **8,582 B** — smaller than estimated because the 16×8
port needs only `keymap[128]`), `es_config_t earthsea` in `nvram_data_t`,
`SCENE_SLOTS` 20 → 18. Built with `dewb/monome-build`:

| Quantity | Measured |
|----------|----------|
| `sizeof(nvram_data_t)` (`.flash_nvram`) | `0x226b4` = 140,980 B (145 K region → **7.3 KB margin**) |
| Code + data image end (LMA) | `0x80056610` (unchanged) |
| NVRAM base | `0x8005bc00` |
| **Free program flash for ES code** | **21,996 B ≈ 21.5 KB** |

Notes discovered: `FIRSTRUN_KEY` is already **0x29** (follower/MIDI bumps) →
Phase 5 bumps to **0x2A**. Ansible quirk: `e.octave` is edited/displayed but
**never applied to pitch** upstream — ported as-is (inert), noted in-file.

---

## 1. Goal & decisions

| # | Decision | Resolution |
|---|----------|-----------|
| — | Integration approach | **Native port** — new `M_EARTHSEA` mode, engine drives TT's 4 CV + 4 TR, live-together with scripts (not i2c to an external Ansible). |
| 1 | Output ownership | **Shared, arbitrated** — `es_suppresses_output(ch)` gate in `tele_tr`/`tele_cv`, same as MP/Kria. ES owns a jack while its voice on that channel is enabled and the app is engaged; released voices go back to scripts. |
| 2 | Persistence | **Global bank** — one `es_config_t` in `nvram_data_t` (like `kria`), Ansible's 8-preset array + glyphs dropped. |
| 3 | Timing model | **Timer-driven, ms-resolution** — ES is a realtime phrase recorder, *not* a stepped sequencer. No `kria_clock`-style dividers; the shell owns a variable-interval one-shot play timer (§7). |
| 4 | Grid | **16×8 primary** (Ansible already supports 8-row; strip the `monome_size_y() == 16` branches). |
| 5 | Mode entry / keys | `alt-E` (free — alt H/M/P/K taken). `Space` play/stop, `A` arm, `X` ext-clock, `S` save, `[`/`]` prev/next pattern (mirrors Ansible's two front-panel keys). |
| 6 | External clock / play triggers | Trigger **input 2** = pattern clock (Kria owns input 1), **input 3** = play/reset trigger. Both port `handler_ESTr`. |
| 7 | Kria/MP coexistence | Same policy as today's Kria↔MP pair: all can be loaded, jack contention is arbitrated per-channel by the ownership gates; user avoids running two apps on the same voice. No forced mutual exclusion. |
| P | **Pattern/scene budget** | **Scenario A** — full 16 patterns, `SCENE_SLOTS` 20 → 18 — unless you choose otherwise at Phase 0. |

---

## 2. What Earthsea is (for the engine port)

A live polyphonic grid instrument plus phrase recorder:

- **Keyboard**: fourths layout, `semitone = x + (rows-1-y)*5 - 1`, clamped
  0–119 → ET pitch. Column 0 is the control strip; key (15,0) is the "rest"
  key.
- **Voices**: up to 4, allocated by "reuse same x/y, else first free, else
  steal oldest" (`es_note_on`). Voice *n* → `CV n` (pitch) + `TR n` (gate).
  Two voice masks: global `e.voices` (live play) and per-pattern
  `p.voices` (playback), editable on the voices view; global octave 0–5.
- **Recorder**: arm → first press starts recording; each key on/off/rest is an
  event with the **interval since the previous event in ms ticks**
  (`get_ticks()` deltas). `ES_CHORD_THRESHOLD = 30` ticks groups events into
  chords. Max 128 events/pattern, 16 patterns.
- **Playback**: timer chain — play event, re-arm timer with the next event's
  interval. Loop flag; non-loop stops at end. While playing, top row scrubs
  (restart at pos/16). Pattern select view (4×4) + press = switch & play.
- **Transforms (runes view)**: reverse (rewrites the event list), double/half
  speed (interval scaling, chord threshold preserved), linearize (all
  intervals become the first supra-threshold interval — metronomic).
- **Edge modes** (per pattern): `PATTERN` (gates as recorded), `FIXED`
  (`edge_time` ms, 16–256, note-off timers), `DRONE` (press toggles).
- **Arp mode**: while on and not recording, a key press sets
  `root_x/root_y` and restarts playback — live transposition of the pattern.
- **Keymap**: with rest key held while stopped, presses cycle a 0–2 shading
  value per key (visual guide only, but user data — persist it).
- **Scale overlay**: `e.scale` (0–15, 16 = off) lights in-scale keys —
  display only, pitch layout is fixed. Reuse the shared MP/Kria scale bank
  instead of Ansible's `f.scale`.
- **ii slave commands** (`ii_es`): PRESET, PATTERN, CLOCK, RESET, STOP,
  TRANS (walk root along the fourths layout), MAGIC (speed/linearize/
  direction), MODE (edge), CV get — this is the op-retarget surface (§8).

Port targets: `es_note_on/off`, `es_record_pattern_note`,
`es_play_pattern_note`, `es_start/stop_playback`, `es_next_note`,
`es_start_recording`, `es_complete_recording`, `es_update_total_time`,
`es_double_speed`/`es_half_speed`/`es_reverse`, `handler_ESGridKey`,
`refresh_es`, `ii_es`, `default_es`.

---

## 3. Architecture — same three-tier split

```
      TT scripts / ES.* ops                keyboard / grid / OLED
             │                                    │
  src/ops/earthsea.c ── teletype_io.h seam ──► module/earthsea_mode.c   ← SHELL
                                                  │  (timers→events, flash,
                       ┌──────────────────────────┼──── ownership, OLED, views)
                       │                          │                    │
              src/es_engine.c              src/es_grid.c        src/es_binding.c   ← PURE
              (patterns, record,           (main/patterns/      (es_output_t →
               playback, voices,            runes/edge/voices    tele_tr/tele_cv/
               transforms)                  render + keys)       kria_i2c fan-out)
                       │                                               │
                       └────────── es_output_t vtable ─────────────────┘
                                                                       │
                                     module/main.c: tele_tr() / tele_cv()  ← HW
```

### New files

```
src/es_engine.{h,c}       es_engine_t {cfg, rt}; note on/off, record, play-step,
                          transforms; es_output_t vtable; config validation
src/es_grid.{h,c}         all 5 views (main / patterns / runes / edge / voices)
                          rendering into a 16×8 LED buffer + key handling
src/es_binding.{h,c}      es_output_t → tele_tr (gates), tele_cv + note→ET
                          (reuse kria_note_to_cv), kria_i2c_set_voice/cv/tr
module/earthsea_mode.{h,c} host shell (§5)
tests/es_tests.c          host tests for the pure engine
```

### Purity rules (the part that differs from Kria)

Ansible's ES calls `get_ticks()` and `timer_add()` *inside* the engine logic.
The port must not:

- **Time is injected.** Every engine entry point that needs wall time takes a
  `uint32_t now` argument (`es_engine_grid_press(e, x, y, z, now)`,
  `es_engine_play_advance(e, now)`, …). The engine stores timestamps/intervals
  but never reads a clock.
- **Timers are inverted.** The engine never schedules. `es_engine_play_advance`
  plays the current event(s) and **returns the next interval in ticks** (0 =
  stopped); the shell re-arms its softTimer with that value. Fixed-edge
  note-offs: `note_on` via the vtable carries an optional `duration`; the
  *binding/shell* schedules the note-off timer and later calls
  `es_engine_note_off_voice(e, v)` — same shape as Kria's deferred
  gate/repeat timers.
- `es_output_t = { note_on(ctx, voice, semitone, duration_ticks),
  note_off(ctx, voice) }` — the only exit path. Only `es_binding.c` includes
  `teletype_io.h`.
- `es_engine_config_valid()` bounds-checks everything on flash load
  (`length ≤ 128`, `p_select ≤ 15`, `edge ∈ {0,1,2}`, `interval_ind < length`,
  voice masks ≤ 0xF, `octave ≤ 5`, `scale ≤ 16`, keymap values ≤ 2), mirroring
  `mp/kria_engine_config_valid`.

---

## 4. Voice / output mapping

- Voice *n* → `CV n` (ET semitone via `kria_note_to_cv`, same tuning as the
  `N` op) + `TR n` (gate). No slew/glide in ES.
- `es_suppresses_output(ch)`: true while the app is engaged (`active ||
  running`) **and** channel *ch* is in the effective voice mask
  (`e.voices | p[sel].voices`). Disabled voices stay scriptable.
- i2c followers: the binding routes every note through the **shared follower
  table** (`kria_i2c_set_voice` → `kria_i2c_cv`/`kria_i2c_tr`), voice *n* =
  follower track *n* — identical to how MP joined Kria's follower layer. The
  shared ii grid view (`kria_i2c_view_*`) is exposed on the same keyboard key
  (`4`) as in Kria/MP.

---

## 5. Module integration seams (the proven 7-file recipe)

**`module/earthsea_mode.h`** — public contract, mirroring `kria_mode.h`:

```c
void set_earthsea_mode(void);
void earthsea_mode_exit(void);
void process_earthsea_keys(uint8_t key, uint8_t mod_key, bool is_held_key);
uint8_t screen_refresh_earthsea(void);
bool es_owns_grid(void);
void es_grid_key(uint8_t x, uint8_t y, uint8_t z);
void es_grid_render(void);
void es_clock_trigger(void);           // trigger input 2 (stepped playback)
void es_play_trigger(void);            // trigger input 3
bool es_suppresses_output(uint8_t ch);
// op seams: es_op_pattern / es_op_reset / es_op_stop / ... (§8)
```

File-static state block like Kria: `initialized / active / running / writing /
dirty / view`, plus the engine, its runtime, and the timer bookkeeping.

**`module/main.c`** — the standard 8 edits:
1. `#include "earthsea_mode.h"`.
2. `set_mode()`: `case M_EARTHSEA: set_earthsea_mode();` + exit hook.
3. `handler_ScreenRefresh()`: `case M_EARTHSEA`.
4. `process_keypress()`: `case M_EARTHSEA`.
5. `process_global_keys()`: `alt-E` toggle.
6. `handler_AppCustom()`: **selectors `data == 3`** (play-timer tick) and
   **`data == 30..33`** (per-voice fixed-edge note-off). MP owns 1; Kria owns
   2, 10–13, 20–23. Don't collide.
7. `handler_Trigger()`: inputs 2/3 → `es_clock_trigger()` / `es_play_trigger()`
   when ES is running.
8. `tele_tr()` / `tele_cv()`: add the `es_suppresses_output(i)` guard beside
   MP's and Kria's.

**`module/grid.c`** — add `G_EARTHSEA` to `grid_control_mode_t`
(grid.c:33), map it in `grid_set_control_mode`, and add the
press/render ownership branches (`es_owns_grid()`), copying the Kria branches.

**`module/globals.h`** — append `M_EARTHSEA` to `tele_mode_t` (after
`M_KRIA`; never renumber).

**`module/help_mode.c`** — `HELP_PAGES` 19 → 20, new EARTHSEA page.

**`module/config.mk`** — add `../module/earthsea_mode.c`, `../src/es_engine.c`,
`../src/es_grid.c`, `../src/es_binding.c` to `CSRCS`; drop `SCENE_SLOTS`
20 → 18 in `flash.h` (Scenario A). `__flash_nvram_size__` stays 145 K.

**`tests/Makefile` + `simulator/Makefile`** — add the `es_*.o` objects; every
new `teletype_io.h` seam gets a `simulator/tt.c` stub.

Timer plumbing (all ISR-safe, copying Kria's pattern): softTimer callbacks
only post `kEventAppCustom`; all engine calls happen in the event loop wrapped
in `writing = true/false`. The blinker (288 ms) and the 25 ms play-position
UI tick don't need own timers — fold both into the existing screen/grid
refresh cadence (a blink counter in the shell, grid marked dirty while
playing).

---

## 6. Persistence — global bank

Copy the `kria` slot pattern in `module/flash.{h,c}`:

```c
// flash.h
nvram_data_t {
    nvram_scene_t scenes[SCENE_SLOTS];   // 20 → 18 (Scenario A)
    ...
    uint8_t scale_bank[MP_SCALE_SLOTS][8];
    kria_config_t kria;
    es_config_t earthsea;                // NEW: single global ES bank
};
void flash_get_es(es_config_t* dst);
void flash_update_es(const es_config_t* src);
```

- First-run: stage `es_engine_set_defaults()` in RAM, one `flashc_memcpy`
  (port of `default_es`, minus the 8-preset loop — note Ansible's own
  `default_es` has an index bug, `e.p[i]` inside the `j` loop; don't copy it).
- **Bump `FIRSTRUN_KEY` 0x25 → 0x26** — NVRAM wipe on upgrade, document the
  scene backup warning (same as the Kria flash).
- Load path runs `es_engine_config_valid()`; invalid → defaults.
- Add/extend the `sizeof(nvram_data_t) <= __flash_nvram_size__` static assert.
- Save trigger: keyboard `S` (like Kria); also flash the follower bank if
  `kria_i2c_take_dirty()`.

---

## 7. Timing (the design-work section — everything else is rote)

The play engine is a self-re-arming one-shot:

1. Shell arms `es_play_timer` for interval *T*.
2. ISR callback posts `kEventAppCustom, data == 3` (and removes the timer).
3. Event handler: `next = es_engine_play_advance(&eng, get_ticks())`; if
   `next > 0`, re-arm for `next`; else playback ended.

**Drift**: re-arming from the event loop adds dispatch latency per event.
Mitigate by keeping an absolute next-deadline in the shell
(`next_deadline += interval; arm(next_deadline - get_ticks(), min 1)`) — cheap
and keeps long patterns honest. Ansible itself re-arms in the ISR, so this is
the one behavioral deviation to watch on hardware.

**Recording** needs no timer at all: intervals are computed from `get_ticks()`
at key-event time (passed in as `now`).

**External clock** (`clock_external`): rising edge on input 2 plays the next
chord group (the `while > ES_CHORD_THRESHOLD` walk from `handler_ESTr`) —
stepped playback, internal timer suspended. Toggled by keyboard `X` /
jack sensing equivalent. Note-position display switches to event-index mode
(`refresh_es` already has both formulas).

**Fixed-edge note-offs**: binding receives `duration_ticks` on `note_on`,
arms per-voice `auxTimer` equivalents → `data == 30+voice` → shell calls
`es_engine_note_off_voice`. Drone/pattern edges pass duration 0.

---

## 8. Ops — retarget the existing `ES.*` i2c ops

`src/ops/earthsea.c` already defines the full family as `MAKE_SIMPLE_I2C_OP`s;
retarget them to the native engine exactly like `KR.*`/`MP.*` were (seam in
`teletype_io.h`, impl in `earthsea_mode.c`, stub in `simulator/tt.c`). No new
tokens needed except `ES.RUN`:

| Op | Native behavior (mirrors `ii_es`) |
|----|-----------------------------------|
| `ES.PATTERN x` | select pattern 0–15 |
| `ES.RESET x` | start playback at scrub position 0–15 |
| `ES.STOP` | stop playback |
| `ES.CLOCK x` | step next chord group (external-clock step) |
| `ES.TRANS x` | walk pattern root ±x along the fourths layout |
| `ES.MAGIC x` | 1 half / 2 double / 3–4 linearize on-off / 5–6 fwd-rev |
| `ES.MODE x` | edge: <0 pattern, 0 drone, 1–15 fixed (x+1)·16 ticks |
| `ES.CV x` | read voice x pitch (ET dac value) |
| `ES.PRESET x` | **no-op or alias of ES.PATTERN** — presets don't exist in the port; pick at Phase 6 |
| `ES.TRIPLE` | leave as-is (original-Earthsea-module i2c op; Ansible ignores it too) |
| `ES.RUN x` | NEW (full registration path): engage/disengage the background engine, like `KR.RUN` |

Update `docs/ops/ansible.toml` (or wherever the ES op docs live) to note the
retarget, as was done for `KR.*`.

---

## 9. Phasing (each phase compiles + is testable)

> **STATUS 2026-07-04: Phases 0-6 SHIPPED (compile-clean, tests green;
> needs hardware validation — Phase 7).** Results per phase below; final
> numbers: ES code (engine+binding+grid+shell+ops) **+11.2 KB** of text
> (image end `0x80056610` → `0x800592c4`), **~10.3 KB program-flash headroom
> left**, NVRAM 140,980 B in the unchanged 145 K region (~7.3 KB margin).
> Tests **146/146** (111 baseline + 35 ES: engine 23 + grid 8 + binding 2 +
> layout/validation). Deviations from this plan as written:
> - `es_config_t` is 8,582 B (keymap[128], 16×8-only; glyphs dropped).
> - `FIRSTRUN_KEY` went 0x29 → **0x2A** (0x25 was stale info).
> - Phase 5 (persistence) landed together with Phase 4 (the shell needed the
>   flash accessors to link).
> - No separate `es_running` flag: engaged := `active || rt.mode == es_playing`;
>   exiting the mode kills live/drone notes unless a pattern is playing.
> - `note_off` is emitted only for active voices (Ansible clears the gate
>   unconditionally; identical at the jack, avoids stray follower note-offs).
> - Keyboard: `Space` play/stop, `A` arm, `X` ext clock, `[`/`]` pattern
>   prev/next (+restart), `S` save, `1`/`4` ES / i2c-follower views, `alt-E`
>   enter. Help page not yet added (deferred with Phase 7 polish).
> - Ops: retargeted MODE/CLOCK/RESET/PATTERN/TRANS/STOP/MAGIC/CV + new
>   `ES.RUN`; PRESET + TRIPLE stay i2c passthrough. `docs/ops/earthsea.toml`
>   rewritten (it described the original-module semantics, not Ansible's).

- **Phase 0 — Budget spike (½ day).** `es_config_t` at Scenario-A size in
  `nvram_data_t`, `SCENE_SLOTS` 18, empty `M_EARTHSEA` stub, build, read the
  map. Go/no-go gate.
- **Phase 1 — Pure engine + tests.** Port record/playback/voices/transforms
  into `src/es_engine.{h,c}` behind `es_output_t` with injected time.
  `tests/es_tests.c`: defaults + validation, fourths-layout pitch, voice
  allocation & oldest-steal, record intervals & chord grouping, loop/non-loop
  end, scrub-position math, reverse (incl. interval reindexing), double/half
  speed, linearize, edge modes, arp root transpose, keymap cycling, TRANS
  walk. Baseline suite is 111/111 — keep it green.
- **Phase 2 — Binding.** `es_binding.c` → `tele_tr`/`tele_cv`/`kria_note_to_cv`
  + `kria_i2c` fan-out. Add to `config.mk` (dead-code-eliminated, image
  unchanged — same trick as Kria Phase 2).
- **Phase 3 — Grid UI.** `es_grid.{h,c}`: main view (keymap/scale/active
  notes/scrub row/control column), patterns+scale view, runes, edge, voices.
  Render smoke tests.
- **Phase 4 — Mode shell + integration.** `earthsea_mode.{c,h}` + the 7-file
  seam edits (§5): timers→events, ownership gates, keyboard, minimal OLED
  (mode/pattern/edge/voice status), alt-E. First flash + hardware smoke test.
- **Phase 5 — Persistence.** `flash_get/update_es`, defaults, validation,
  `FIRSTRUN_KEY` bump, `S` to save.
- **Phase 6 — Ops.** Retarget table above + `ES.RUN` + simulator stubs + docs.
- **Phase 7 — Hardware soak.** Timing feel vs a real Ansible/Earthsea if
  available (recording quantization feel, edge-fixed lengths), jack sharing
  with scripts and with Kria/MP, followers, ext clock, 3-app concurrency.

---

## 10. Risks & watch-items

- **Timer-driven playback via the event loop** is the one novel mechanism —
  MP/Kria are fixed-period, ES re-arms with a different interval every event.
  Watch for drift/jitter on hardware; the absolute-deadline mitigation (§7) is
  cheap insurance. Worst case, move the re-arm into the ISR callback (compute
  next interval in the previous event-loop pass and stash it).
- **Tick source**: Ansible intervals are `get_ticks()` ms. Teletype's shell
  must use the same 1 ms tick base or recorded patterns will play at the
  wrong speed. Verify against `kria_mode.c`'s timer usage.
- **NVRAM wipe + scene cut** (20 → 18): the param-knob scene sweep already
  derives from `SCENE_SLOTS` (commit `bbbf401`), but re-audit for any other
  hardcoded scene-count assumptions.
- **AppCustom selector space** is getting crowded (MP 1, Kria 2/10–13/20–23,
  ES 3/30–33). Consider a small enum header shared by the three shells so
  collisions become compile-visible.
- **Three background apps, four jacks**: with MP + Kria + ES all resident,
  per-channel arbitration is now three gates deep in `tele_tr`/`tele_cv`.
  Order matters only for who wins a conflict — document "last engaged app
  wins" or keep it simple: gates are independent, users shouldn't run two
  apps on one channel.
- **256-grid code paths**: strip the `monome_size_y() == 16` branches
  deliberately (bottom-half runes view, 256 pattern hotkeys on column 0,
  quadrant flags) — half-porting them is worse than dropping them.
- **`es_reverse` uses a 512 B stack temp** (`es_event_t te[128]`) — fine, but
  it runs in the event loop; keep it off any ISR path.
- **Ansible quirks — don't port the bugs**: `default_es` writes `e.p[i]`
  where it means `e.p[j]`; `is_arm_pressed()` has a misplaced `break`
  (checks only the first held key). Replicate the *intent*, note the
  divergence in-file.
- **Simulator parity**: every new seam needs a `simulator/tt.c` stub or the
  test build breaks (standing rule).
- **Shared-object gotcha**: host tests and the AVR32 build share `src/*.o` —
  clean between a `make test` and a module build.
