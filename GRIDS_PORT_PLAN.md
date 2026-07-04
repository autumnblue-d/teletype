# Plan: Grids topographic drum ops in Teletype

Implementation plan for porting Mutable Instruments **Grids** (topographic drum
sequencer) into the modded Teletype firmware as a small set of **stateless
scripting ops** — the same class of feature as the existing `DR.P` / `DR.T` /
`DR.V` drum ops, *not* a native grid mode like Kria/Earthsea/Meadowphysics.

- Source: `~/git/eurorack/grids` (Mutable Instruments, AVR8). Only two files
  matter: `resources.cc` (the 25-node drum map) and `pattern_generator.cc`
  (the `ReadDrumMap` interpolation + `EvaluateDrums` threshold logic).
- Target: this repo, branch `kria` (or a new `grids` branch off it).
- Precedent: the shipped `DR.*` ops (`src/ops/maths.c` + data in `src/table.c`
  + `src/drum_helpers.c`). Grids is the **same shape** — a const pattern-data
  table plus a small pure lookup — so this plan reuses those five registration
  seams verbatim. No new mode, no OLED editor, no NVRAM changes.

---

## 0. The flash reality — measured from the current build

From the current `module/teletype.elf` / `teletype.map` (post MP + Kria + i2c
followers + MO/I2M + Earthsea):

| Quantity | Value |
|----------|-------|
| Program image end (LMA of `.flash_nvram`) | `0x800592c4` |
| NVRAM region base (`__flash_nvram_size__ = 145K`) | `0x8005bc00` |
| **Free program flash** | **~10.5 KB** |
| `sizeof(nvram_data_t)` (`.flash_nvram` size `0x226b4`) | 140,980 B |
| Free NVRAM margin (145 K region = 148,480 B) | ~7.3 KB (≈ 1 scene slot) |

> ⚠️ **Measure with `-fshort-enums`.** `module/config.mk` builds with
> `-fshort-enums -fno-common`; a host `sizeof` probe without those flags
> overstates command structs ~1.5×. See `MEMORY_USAGE.md`.

### What Grids costs (estimate)

**Scope decision: topographic drum map only.** Drop Grids' internal clock
(`clock.cc` + `lut_res_tempo_phase_increment`, 2 KB — Teletype drives its own
step counter), the euclidean fill mode (`lut_res_euclidean`, 4 KB — optional,
see §6), swing, output routing, and the whole `PatternGenerator` runtime state
machine. Port the map + `ReadDrumMap` as a **pure function**.

| Component | Bytes | Basis |
|---|---|---|
| 25 nodes × 96 B (drum map data → `.rodata`) | 2,400 | source, exact |
| `drum_map[5][5]` pointer table | 100 | 25 × 4 |
| `grids_level` + `u8mix` + trigger/accent logic | ~600 | est. (cf. `tresillo`=244 B) |
| 3 op wrappers + op structs + name strings | ~350 | est. (cf. `op_DR_*`) |
| `tele_ops[]` pointer entries (3 × 4) | 12 | |
| Help text (3 ops) | ~500 | est. |
| `match_token.rl` tokenizer (3 tokens) | ~150 | est. |
| **TOTAL** | **≈ 4.1 KB** | fits current ~10.5 KB headroom, leaves ~6 KB |

**No RAM cost, no NVRAM cost:** the map is `const` (flash-only), the ops are
stateless, and nothing is added to `scene_state_t` or `nvram_data_t`. The flash
format is unchanged, so **`FIRSTRUN_KEY` is NOT bumped** and existing scenes
survive the upgrade.

---

## 1. Op design (the public API)

Grids interpolates a level 0–255 for each (instrument, step) from a 2-D map
position (x, y), then triggers when `level > density-threshold`. Expose that as
three pure, stateless get-ops in a new `GR.*` namespace (verified free in
`match_token.rl`):

| Op | Params | Returns | Semantics |
|---|---|---|---|
| `GR.P i x y d s` | 5 | 0/1 | **trigger**: 1 if instrument `i` (0–2 = BD/SD/HH) fires at step `s` (0–31) for map position `x`,`y` (0–255) at density `d` (0–255) |
| `GR.L i x y s` | 4 | 0–255 | **raw level** from the interpolated map (no density gate) — for CV / probability |
| `GR.A i x y d s` | 5 | 0/1 | **accent**: 1 if the fired level > 192 (Grids' accent rule) |

Design mirrors `DR.P B P S` (stateless, caller supplies the step) so it slots
into existing `M` / metro / `SCRIPT` idioms with no runtime state. Clamp/`wrap`
all args defensively exactly as `drum()` does (`src/drum_helpers.c`).

Naming/param conventions to confirm before coding:
- `i`, `s`, `x`, `y`, `d` order and ranges as above (bikeshed-able).
- Whether `GR.L` is worth the flash (~60 B) or folded into `GR.P` with a
  threshold arg. Recommend keeping it — cheap and useful for CV.

---

## 2. File layout

Follow the `drum_helpers` precedent (helper TU + data in a table TU + ops in an
op TU):

- **`src/grids_data.c` / `.h`** — the 25 node arrays (`const uint8_t node_N[96]`)
  and `const uint8_t *const grids_map[5][5]`. Transcribed mechanically from
  `resources.cc:243–620` (strip `PROGMEM` / `prog_uint8_t` → plain `const
  uint8_t`; `pgm_read_byte(p)` → `*(p)`). ~2.5 KB `.rodata`.
- **`src/grids_helpers.c` / `.h`** — the pure algorithm:
  ```c
  // linear interp, Mutable's U8Mix
  static inline uint8_t u8mix(uint8_t a, uint8_t b, uint8_t bal) {
      return (a * (255 - bal) + b * bal) >> 8;
  }
  // bilinear read of the 5x5 map; instrument 0..2, step 0..31, x/y 0..255
  uint8_t grids_level(uint8_t instrument, uint8_t step, uint8_t x, uint8_t y);
  int grids_trigger(int instrument, int x, int y, int density, int step);
  int grids_accent (int instrument, int x, int y, int density, int step);
  ```
  `grids_level` is `ReadDrumMap` verbatim (`pattern_generator.cc:78–95`), minus
  PROGMEM. `grids_trigger`/`grids_accent` are the `EvaluateDrums` core
  (`level > ~density` → hit; `level > 192` → accent), minus perturbation/swing.
- **Ops** go in a new block in **`src/ops/maths.c`** next to the `DR.*` ops
  (they are conceptually the same family), or a dedicated `src/ops/grids.c`.
  Recommend `maths.c` to avoid a new op TU and keep the diff small.

Constants: `kNumParts = 3`, `kStepsPerPattern = 32` (from
`pattern_generator.h`) → `#define GRIDS_INSTRUMENTS 3`, `GRIDS_STEPS 32`.

---

## 3. Algorithm port (verbatim, de-AVR8'd)

`ReadDrumMap` (the whole thing):

```c
uint8_t grids_level(uint8_t instrument, uint8_t step, uint8_t x, uint8_t y) {
    uint8_t i = x >> 6, j = y >> 6;              // 0..3 cell index
    const uint8_t *a = grids_map[i][j];
    const uint8_t *b = grids_map[i + 1][j];
    const uint8_t *c = grids_map[i][j + 1];
    const uint8_t *d = grids_map[i + 1][j + 1];
    uint8_t off = instrument * GRIDS_STEPS + step;
    return u8mix(u8mix(a[off], b[off], x << 2),
                 u8mix(c[off], d[off], x << 2), y << 2);
}
```

Note `grids_map` is `[5][5]` (needs `i+1`/`j+1` up to index 4). Clamp `x`,`y`
to 0–255 and `instrument`/`step` to range before calling so the `>>6` / offset
math can't index out of bounds.

Trigger/accent (from `EvaluateDrums`):

```c
int grids_trigger(int inst, int x, int y, int density, int step) {
    /* clamp args */
    uint8_t level = grids_level(inst, step, x, y);
    uint8_t threshold = ~(uint8_t)density;      // Grids: threshold = ~density
    return level > threshold;
}
// accent: same, then `level > 192`
```

Perturbation/randomness is **dropped** in v1 (it needs per-part RNG state,
which breaks the stateless model). If wanted later, reuse Teletype's
`random_state_t` — see §6.

---

## 4. Data transcription

`resources.cc` node arrays are plain decimal `uint8_t`, 96 per node, already in
`instrument*32 + step` order. Transcribe with a throwaway script (no hand
editing 2,400 numbers):

```
awk/python: extract node_0..node_24 bodies from resources.cc,
emit `const uint8_t node_N[96] = { ... };` + the grids_map[5][5] table
```

`grids_map[5][5]` initialiser is Grids' own `drum_map[5][5]`
(`pattern_generator.cc:69–76`) — copy that ordering exactly (it is **not** just
`node_0..24` in sequence; preserve the published layout so patterns match the
hardware).

Licensing: Grids is MIT (Émilie Gillet). Keep the copyright header in
`grids_data.c` / `grids_helpers.c`.

---

## 5. Registration checklist (the five seams)

Per `CLAUDE.md` "Adding a New OP":

1. **`src/ops/maths.c`** — add `op_GR_P_get` / `op_GR_L_get` / `op_GR_A_get`
   static fns + `const tele_op_t op_GR_P = MAKE_GET_OP(GR.P, op_GR_P_get, 5,
   true);` (and L=4, A=5). Declare them `extern` where `op_DR_*` are declared
   (grep shows they surface via the ops headers).
2. **`src/ops/op.c`** — add `&op_GR_P, &op_GR_L, &op_GR_A` to `tele_ops[]`
   (next to `&op_DR_*`, line ~100).
3. **`src/ops/op_enum.h`** — **do not edit**; run `python3 utils/op_enums.py`
   to regenerate `E_OP_GR_P` etc.
4. **`src/match_token.rl`** — add near the `DR.*` block (line ~263):
   ```
   "GR.P" => { MATCH_OP(E_OP_GR_P); };
   "GR.L" => { MATCH_OP(E_OP_GR_L); };
   "GR.A" => { MATCH_OP(E_OP_GR_A); };
   ```
   Order longer/more-specific tokens correctly (all same length here).
5. **`module/config.mk`** — add `grids_data.c` + `grids_helpers.c` to `CSRCS`.
6. **`tests/Makefile`** *and* **`simulator/Makefile`** — add
   `../src/grids_data.o` + `../src/grids_helpers.o` to the object lists
   (alongside `drum_helpers.o`).
7. **`module/help_mode.c`** — add a `GR.*` help block (3 ops), mirroring the
   `DR.*` block at lines 283–298.

Ragel: `match_token.rl` → `match_token.c` regenerates automatically via the
test/sim Makefiles; the module build needs `ragel` present.

---

## 6. Optional extensions (deferred, not in v1)

- **Euclidean fill** (`GR.E len fill step`): port `lut_res_euclidean` (+4 KB
  `.rodata`) + a 1-line lookup. Only if flash allows — with current ~10.5 KB
  headroom, v1 (~4 KB) + euclidean (~4 KB) leaves ~2 KB, too tight. Do the
  `disting` strip (frees ~12.6 KB, see analysis) first if both are wanted.
- **Randomness/perturbation** (`GR.PR i x y d s seed`): adds a `seed` arg and
  reuses `random_state_t`; keeps ops stateless. ~negligible flash.
- **Accent-as-level bit** could be merged into `GR.P` return (bit 1 = accent)
  to save one op (~60 B) at the cost of a fussier API.

---

## 7. Testing

- **`tests/grids_helpers_tests.c`** (new, registered in `tests/Makefile` like
  `drum_helpers_tests.o`): assert `grids_level` against a handful of known
  values read straight from `node_0`/corner cells (e.g. step 0, inst 0,
  x=y=0 → 255; interpolation midpoints). Assert `grids_trigger` threshold
  behavior at density 0 / 255 and clamping at out-of-range args.
- `cd tests && make test` — also runs the registration self-check that every
  op in `tele_ops` has an enum + token entry (catches a missed seam).
- **Shared-object gotcha:** host tests and the AVR32 build share `src/*.o`;
  `make clean` between them (see project memory).
- Manual: on hardware, `GR.P 0 X Y 128 I` inside a metro with `I` stepping
  0–31 should reproduce the classic Grids kick pattern as X/Y sweep the map.

---

## 8. Risks & notes

- **`drum_map[5][5]` ordering** is the one correctness trap — copy Grids'
  table literally; a wrong permutation yields plausible-but-wrong patterns.
- **`u8mix` overflow**: `a*(255-bal)` fits in 16 bits (255×255); use `int`/
  `uint16_t` intermediates. AVR32 is 32-bit so this is free.
- **No flash-format change** → no `FIRSTRUN_KEY` bump, no scene migration.
- **Flash budget is the gate**: v1 fits, but it consumes ~4 KB of the ~10.5 KB
  headroom. If more features are planned soon, reclaim flash first
  (`disting` strip ≈ 12.6 KB) rather than spending the last of the margin.

---

## 9. Effort

Small — comparable to the original `DR.*` ops. Estimated:
- Data transcription (scripted): ~0.5 h
- `grids_helpers.c` + ops + registration: ~1–2 h
- Tests + help + on-device check: ~1 h

No new mode, no OLED, no NVRAM, no serialization — the smallest possible
addition surface.
