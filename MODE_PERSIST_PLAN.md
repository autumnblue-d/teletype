# MODE_PERSIST_PLAN.md — unify save UX across MP / KR / ES

## §0 Goal & guiding principle

Meadowphysics (MP), Kria (KR), and Earthsea (ES) each persist to flash with a
different interaction contract. The **storage scope** difference (MP per-scene;
KR/ES single global banks) is memory-forced and correct — do **not** change it.
Unify only the *interaction contract* so the three feel identical:

1. one shared commit path (kills per-mode drift),
2. one explicit save gesture with visible confirmation,
3. a scope badge so per-scene vs global is legible instead of surprising,
4. "save scene" as a universal commit point (uniform durability rule).

Nothing here alters the NVRAM layout, `FIRSTRUN_KEY`, or `SCENE_SLOTS`.

## §1 Current state (the divergences)

| | MP | KR | ES |
|---|---|---|---|
| Scope | per-scene (`scene_state.mp`) | global bank `f.kria` | global bank `f.earthsea` |
| Save helper | none (writes back to `scene_state.mp`) | none — inlined ×2 | `em_save_flash()` ✔ |
| Explicit gesture | none | `HID_S` (kria_mode.c:560) | `HID_S` (earthsea_mode.c:379) |
| Confirmation | none | none | none |
| Dirty flag | `mp_bank_dirty` (bank only) | `cfg_dirty` | `cfg_dirty` |
| Committed to flash | only on scene save | mode exit / `HID_S` | mode exit / `HID_S` |

Key facts (from source):

- Mode dispatch + exit hooks: `main.c:836 set_mode()` → `*_mode_exit()` at
  838/842/845. Keyboard dispatch `process_keypress()` `main.c:907`.
- Every `process_*_keys(key, mod_key, is_held_key)` early-returns on
  `is_held_key` — **no chorded/hold gestures**; single keypress via an
  `if/else if` ladder of `match_no_mod/…` (`keyboard_helper.h`).
- Scene commit = `flash_write()` (`flash.c:118`); it persists `scene->mp` at
  `flash.c:130`. Two **interactive** call sites: `preset_w_mode.c:99` (alt-Enter)
  and `grid.c:844` (front-panel preset-write confirm). Bulk USB import at
  `usb_disk_mode.c:263` is not interactive.
- **No OLED confirmation/notification mechanism exists anywhere** in `module/`.
- Render fns draw into `region line[8]` via
  `font_string_region_clip(&line[n], str, x, y, fg, bg)`;
  `font_string_region_clip_right` (`font.h:65`) right-justifies.
  Headers: KR `line[0]` KRIA@0 / view@54 / RUN-STOP@100 (kria_mode.c:792);
  ES EARTHSEA@0 / mode@100 (earthsea_mode.c:530); MP `MEADOWPHYSICS`@0 with
  P/C/F tabs @104/113/122 (meadowphysics_mode.c:438) — line[0] right is full.
- `scene_text` (globals.h:16) and `preset_select` (globals.h:19, = active slot)
  are module-global externs, reachable from any mode.
- Duplicated verbatim in all three: the i2c-follower flush (`*_flush_i2c`,
  MP:71 / KR:77 / ES:67) and the i2c-oled keyboard-capture preamble at the top
  of each `process_*_keys` (MP:326 / KR:531 / ES:350).

## §2 Phase 1 — shared persistence helper

New `module/mode_persist.{c,h}`. Add `mode_persist.c` to `module/config.mk`
`CSRCS`. Module-only UI glue — **no** OP-registration, no `tests/`/`simulator/`
Makefile entries, no `op_enums.py`.

Lightweight descriptor each mode fills once; shared code owns the dirty-gate,
commit sequencing, and confirmation trigger so the three cannot drift:

```c
typedef struct {
    const char *scope_label;   // "GLOBAL" (KR/ES) or "SCENE" (MP)
    bool  per_scene;
    bool *dirty;               // -> the mode's cfg_dirty / mp_bank_dirty
    void (*commit)(void);      // the mode's flash-write body (see below)
} mode_persist_t;

// If dirty (incl. shared bank / i2c): run commit(), clear flags,
// fire the "SAVED" confirmation. Returns true if anything was written.
bool mode_persist_commit(const mode_persist_t *d);
```

Refactor prerequisites (bring all three to ES's shape):

- **KR:** extract the inlined save (kria_mode.c:236-238 and :561-565) into a
  single `km_save_flash()` mirroring `em_save_flash`; register `commit =
  km_save_flash`.
- **ES:** register `commit = em_save_flash` (already exists).
- **MP:** `commit` = write `scene_state.mp = mp_eng.cfg` + `mp_flush_bank()` +
  full current-scene save (see §3); `dirty` = `mp_bank_dirty` **or** a
  config-changed check (`memcmp(&mp_eng.cfg, &scene_state.mp, …) != 0`).

Route `kria_mode_exit` / `earthsea_mode_exit` / `meadowphysics_mode_exit`
through `mode_persist_commit(&desc)` instead of hand-rolled `if (dirty){…}`.

Load bodies stay per-mode (MP loads from the scene, KR/ES from the global
bank), each keeping its `*_config_valid` / `*_set_defaults` sanitize step —
already uniform.

**Bonus dedup (optional, same file):** hoist the identical `*_flush_i2c` and
the i2c-oled keyboard preamble into shared helpers. Not required for the UX
goal but removes the largest verbatim duplication.

## §3 Phase 2 — uniform gesture + OLED confirmation

**Confirmation helper** (in `mode_persist.c` — nothing exists today):

```c
void mode_confirm_show(const char *msg);            // stamps msg + get_ticks()+~1s
bool mode_confirm_active(const char **out_msg);     // render fns query this
```

Backed by `static char msg[8]; static uint32_t until;`. `mode_persist_commit()`
calls `mode_confirm_show("SAVED")`. Auto-clear: piggyback each mode's existing
periodic timer (`kriaBlinkTimer` kria_mode.c:222, `esBlinkTimer` ES:42, MP's
equivalent) to set `dirty = true` when the banner expires so the next refresh
erases it.

**Gesture parity:**

- KR / ES: keep `HID_S`, but route it through `mode_persist_commit()` so they
  gain the confirmation flash (today they save silently).
- MP: add an `HID_S` handler (currently none). MP's data lives in the scene, so
  `HID_S` performs a real current-scene save — identical to preset_w:
  ```c
  else if (match_no_mod(mod_key, key, HID_S)) {
      scene_state.mp = mp_eng.cfg;
      mp_flush_bank();
      flash_write(preset_select, &scene_state, &scene_text);
      flash_update_last_saved_scene(preset_select);
      mode_confirm_show("SAVED");   // via mode_persist_commit
      dirty = true;
  }
  ```
  This makes "S = my work is now in flash" true in every mode. (`preset_select`
  = active slot; `scene_text` is a global — both confirmed reachable.)

## §4 Phase 3 — scope badge (shared status cell)

One **status cell** per mode, drawn right-justified via
`font_string_region_clip_right`. Normal state = scope badge
(`"GLOBAL"` for KR/ES, `"SCENE n"` for MP, `n = preset_select`); on commit it
flashes `"SAVED"` for ~1s (Phase 2), then reverts. Same slot, two states —
so the badge costs no extra screen space and coexists with the transient
confirmation.

Placement per mode:

- MP: persistent `S<n>` (active scene number) at `line[0]` x=82, in the gap
  before the P/C/F tabs. This is the informative half -- it tells you which
  slot a save lands in. **Shipped.**
- KR / ES: a persistent `GLOBAL` label was **intentionally dropped** (decision,
  2026-07-04). Their `line[0]` is full (title + view/i2c + RUN/STOP or mode
  name), leaving no clean slot, and the label carries little information: it
  never changes, and the absence of a scene number plus the `SAVED` flash
  already imply "not scene-bound." Revisit only if a hardware pass shows the
  scope is genuinely unclear.

Confirmation rendering (all three): the title cell shows the transient message
when `mode_confirm_active()`, else the normal mode title.

## §5 Phase 4 — scene save = universal commit point

Make "save scene" guarantee *everything* is in flash, so durability no longer
depends on which mode you were last in. Semantics of the global banks are
unchanged — a scene save just commits their current state; it does not tie them
to the scene.

- Add thin public wrappers: `bool kria_flush_if_dirty(void)` /
  `bool earthsea_flush_if_dirty(void)` (each = `mode_persist_commit` on its
  descriptor).
- Add `mode_persist_flush_all_dirty(void)` = flush whichever of
  {KR bank, ES bank, shared scale bank, i2c bank} report dirty.
- Call it right after `flash_write` at the two interactive save sites:
  `preset_w_mode.c:99` and `grid.c:844`. (Leave `usb_disk_mode.c:263` bulk
  import untouched.)
- Keep `flash_write()` itself pure (no side effects) so first-run/internal
  writes are unaffected.

## §6 Risks & notes

- **Flash stall:** a scene save may now also write ~27 KB (KR ~18.5 + ES ~8.6)
  when both banks are dirty. Only-when-dirty gating bounds this; document the
  possible brief stall as Kria's exit comment already does (kria_mode.c:233).
  Prefer saving while stopped.
- **Heap/OLED gotcha:** the confirmation adds one small static buffer — no
  bearing on the malloc'd OLED line regions (blank-screen bug). Safe.
- **Shared-`.o` gotcha:** this is module UI glue, not engine logic, so no host
  tests touch it. If any part is later unit-tested, clean between the host-test
  and AVR32 builds (shared `src/*.o`).
- **`config.mk`:** the only registration needed is adding `mode_persist.c` to
  `CSRCS`.

## §7 Sequencing

1 → 2 → 3 → 4. Phase 1 is the enabler; Phases 2–4 are each independently
shippable and testable in the module build (`docker run … 'cd module && make'`).
Verify on hardware or in the simulator per mode after each phase.

## §8 Resolved decisions

- **A (MP save gesture):** feasible — `preset_select` + `scene_text` are globals,
  so MP `HID_S` does a genuine current-scene save (§3). Chosen over a "staged,
  saved-on-scene-save" fallback because it gives literal gesture parity.
- **B (badge placement):** the transient `SAVED` confirmation reuses each
  mode's title cell (always present, never collides). Scope: MP shows a
  persistent `S<n>` in the `line[0]` gap before its tabs; KR/ES omit a `GLOBAL`
  label (see §4 -- low information value, no clean slot).

## §9 Status (as shipped, 2026-07-04)

- All four phases implemented; AVR32 firmware builds clean (exit 0). RAM
  headroom ~16.5 KB (`__heap_end__` 0x16000 − `_end` 0x11f78), well above the
  ~8.3 KB screen-blank floor.
- **Not runtime-verified.** This is module UI: driving the OLED banner and key
  gestures needs the physical module (the `simulator/` is a command parser, not
  a grid/OLED sim). Build + static checks are the ceiling in this environment;
  verify the banner + MP `S<n>` placement on hardware.
- **clang-format version spread:** local v21, build image v14, CI uses
  ubuntu-latest's default -- three versions. `make format` was NOT run (it
  diffs vs `main` with v21 and churns unrelated committed files). New files and
  edited kria/mp/grid/preset_w are v14 whole-file clean; the earthsea diff is
  functional-only. `earthsea_mode.c` has pre-existing v14 format drift at HEAD
  (committed with a different version than kria/mp) -- not introduced here. Run
  the canonical CI formatter before committing.
