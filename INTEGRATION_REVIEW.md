# Integration architecture review: Kria / Meadowphysics / Earthsea

Review of how the three ported grid apps plug into Teletype, and ranked
simplification opportunities. Flash budget (~2.8 KB free program flash) is the
governing constraint — nearly every item below *removes* duplicated code, so it
buys flash rather than spending it. The one item that could go either way (a
full dispatch vtable) is flagged.

## How it's wired today

Each app is a 4-layer stack with a consistent split:

```
src/*_engine.c   pure algorithm (no hardware), injected RNG + output vtable
src/*_binding.c  vtable impl: maps engine semitone/gate output -> tele_tr/tele_cv/i2c
src/*_grid.c     pure grid render/key: (engine*, grid_state*, led[], ...) -> no globals
src/*_clock.c    phase/period arbitration (Kria, MP only)
module/*_mode.c  the shell: owns the engine instance, timers, OLED, flash, op seams
```

The engine / binding / grid *pure* layers are genuinely well-factored — no
hardware globals, host-testable, and ES already reuses Kria's i2c fan-out and
`kria_note_to_cv` instead of copying them. Duplication is concentrated in two
places: **(1) the near-clone binding/clock leaf files**, and **(2) the module
shell + central dispatch**, where the three apps are enumerated by hand at
**seven** sites.

## Tier 1 — clear wins (remove code, near-zero risk)

1. **Collapse `note_to_cv` from 3 copies to 1.** `note_number_to_volts()`
   (`src/ops/maths.c:374`), `kria_note_to_cv()` (`kria_binding.c:10`) and
   `mp_note_to_cv()` (`meadowphysics_binding.c:10`) are the *same function* —
   `#define table_n ET` (`src/table.h:7`) makes maths.c's table identical to the
   bindings' `ET[]`, same ±127 clamp, same negate. ES already reuses Kria's.
   Extract one `note_to_cv()` and have all three call it. **[IMPLEMENTED]**

2. **Merge the two clock files into one shared core.** `kria_clock.c` and
   `meadowphysics_clock.c` share byte-identical `clamp_period`, `*_init`,
   `*_set_period`, `*_set_external`, `*_internal_fire`, `*_external_edge`, and an
   identical struct — differing only in the `KR_`/`MP_` period constants. One
   `grid_clock_t` + shared functions parameterized by min/max/default unifies
   them; the app-specific tails (Kria's `scale_duration`/`repeat_ticks`, MP's
   `period_from_rough_fine`) stay as free functions. **[IMPLEMENTED]**

3. **Delete dead code.** `kria_grid_key_hold` (`src/kria_grid.c:913`, declared
   `kria_grid.h:75`) is never called anywhere in `module/`. Dropped it (plus the
   two comments that referenced it). **[IMPLEMENTED]**

4. **Fold `bind_tr`/`bind_cv` boilerplate.** After #1, Kria and MP's `bind_tr`
   are byte-identical and `bind_cv` differs only in which (now-shared)
   `note_to_cv` they call. Extracted `grid_bind_tr`/`grid_bind_cv` into a shared
   `src/grid_binding.c` that both engine output vtables point at (their `tr`/`cv`
   pointer types are identical); the per-app verbs `cv_slew` (Kria) / `cv_gate`
   (MP) stay local. Fallout: `kria_mode.c` no longer needs `kria_binding.h`
   (`kria_output_t` comes from `kria_engine.h`; the mode uses its own vtable).
   The AVR32 linker was already GC'ing/folding these, so this is a
   source-clarity win, not a flash win. **[IMPLEMENTED]**

## Tier 2 — shared helpers (remove code, low risk)

5. **One `grid_led_finalize(led, vari)` helper.** The clamp-to-15 +
   non-varibright "force full" loop was copy-pasted in 5 places (plus a 6th
   found during the work): `kria_grid.c`, `es_grid.c`, `meadowphysics_grid.c`
   (×2), `kria_mode.c`, and `kria_i2c.c` (the shared i2c view). Added
   `static inline grid_led_finalize()` in new `src/grid_led.h` (inline header,
   no link dep so pure-src and module both use it); all six now call it. This
   also fixes MP, whose copies omitted the `>15` clamp. **[IMPLEMENTED]**

6. **Shared brightness ramp.** `4/8/12` was redefined under four names
   (`L0/L1/L2` in `kria_grid.c`, `MP_LED_DIM/MED/BRI` in `meadowphysics_grid.c`,
   `KM_LD/KM_LB` in `kria_mode.c` and again in `kria_i2c.c`). Defined
   `GRID_L0/L1/L2` once in `grid_led.h`; each file now aliases its local names to
   those, so the ramp has a single source of truth while local readability is
   kept. **[IMPLEMENTED]**

7. **Deduplicate the i2c-follower flush.** `km_flush_i2c` / `mp_flush_i2c` /
   `em_flush_i2c` were byte-identical. Collapsed into one
   `mode_flush_i2c_if_dirty()` in `module/mode_persist.c` (the existing shared
   cross-mode persistence unit); all three modes and their call sites now use it,
   and the three static copies are deleted. **[IMPLEMENTED]**

8. **Shared i2c-view + OLED-editor scaffolding.** Four constructs were
   triplicated across the mode shells and are now single functions in
   `module/mode_persist.c`: the key-handler "editor owns the keyboard" block →
   `mode_i2c_oled_handle_key()`; the `screen_refresh` "editor owns the screen"
   block → `mode_i2c_oled_render_active()`; the grid i2c-view key plumbing →
   `mode_i2c_view_grid_key()`; and the `*_num()` itoa helper → `mode_draw_num()`.
   All three modes now call these. (The one-line confirm-banner header idiom was
   left inline — a per-app title string, nothing to share.) **[IMPLEMENTED]**

## Tier 3 — the dispatch fan-out (structural; measure flash)

Adding/removing an app today means editing **seven** hand-enumerated sites, all
iterating the same three apps in the same order:

| Site | location |
|---|---|
| Screen refresh | `switch(mode)` `main.c:541` |
| Keypress | `switch(mode)` `main.c:931` |
| set_mode enter/exit | `main.c:836` |
| AppCustom (clock/note-off/repeat) | if-chain on magic ints `main.c:582` |
| Trigger inputs | if-chain `main.c:504` |
| Output suppression | 3 calls in `tele_tr`+`tele_cv` `main.c:1166`,`1206` |
| Grid key/render | 2 cascades `grid.c:1066`,`1433` |

**8a — `tele_app_t` registry for the *uniform* seams. [IMPLEMENTED]**
`{owns_grid, grid_key, grid_render, suppresses_output}` in new
`module/tele_app.{h,c}`; a `static const tele_app_t tele_apps[3]` iterated once
now drives the two grid cascades (`grid.c`) and the two output-suppression
cascades (`main.c` `tele_tr`/`tele_cv`). `grid.c` dropped its three per-app mode
includes for the single `tele_app.h`. Adding/removing an app here is now one
table row instead of four edit sites.
**Flash: measured +44 bytes** (`0x58846` → `0x58872`) — the const
function-pointer table costs slightly more than the inline if-chains it
replaced, so the "neutral-to-positive" guess didn't hold. Small, and the session
net is still well positive, but it *is* a cost, not a saving.

Did *not* force a full vtable over everything: the `switch(mode)` sites also
cover the 6 built-in modes, and the AppCustom/trigger sites are genuinely
per-app-shaped (Kria 3 services + 1 trigger; ES 2 + 2; MP 1 + 1).

**8b — AppCustom magic-number encoding. [IMPLEMENTED]** The bare integers
(`1`=MP clock, `2`=Kria clock, `10-13`=note-off, `20-23`=repeat, `3`=ES play,
`30-33`=ES note-off) are now named `#define`s in the mode headers
(`MP_APPEVT_CLOCK`, `KR_APPEVT_CLOCK`/`_NOTEOFF_BASE`/`_REPEAT_BASE`,
`ES_APPEVT_PLAY`/`_NOTEOFF_BASE`), shared by the posting ISR timers and the
`main.c` dispatcher. The per-track/voice ranges are bounded by
`KRIA_NUM_TRACKS`/`ES_NUM_VOICES` (the mode headers now include their engine
header for the count), so the range width has a single source of truth.

## Leave alone — real divergences, not accidental

- **ES output is note-oriented** (`note_on(voice, semitones, duration)`) vs
  Kria/MP's jack-oriented (`tr` + `cv`). ES is a polyphonic keyboard.
- **ES has no `running` flag** (`es_engaged()` derives it); Kria/MP carry
  explicit `*_running`.
- **Blink periods differ** (Kria 100 ms per-track, ES 288 ms single) — intentional.
- **Kria ops live in `ansible.c`** while MP/ES got their own `src/ops/*.c`;
  moving them is cosmetic.
</content>
