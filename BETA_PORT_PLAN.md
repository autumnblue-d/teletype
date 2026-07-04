# Plan: Marbles Beta-distribution shaping op in Teletype

> **STATUS: implemented (Option A, both ops).** `src/beta.{c,h}`, ops `BETA` /
> `DV.B` in `maths.c`, `tests/beta_tests.c` (4 tests, passing). Measured on an
> AVR32 firmware build: **+2,344 B (2.29 KB)** — matching the estimate below —
> leaving **2.76 KB** free program flash.

Feasibility + implementation plan for porting Mutable Instruments **Marbles**'
beta-distribution voltage shaper (the "spread / bias" character of the X
section) into Teletype. This is the natural companion to the already-shipped
déjà vu port ([DEJAVU_PORT_PLAN.md](DEJAVU_PORT_PLAN.md)): in Marbles the X
output is literally `BetaDistributionSample(u, spread, bias)` where `u` is the
uniform value coming *out of* the déjà vu `RandomSequence`
(`random/output_channel.cc:85`). So this op closes the loop — it turns déjà vu's
flat `[0,1)` stream into Marbles' signature bell-with-fat-tails distribution.

- Source: `~/git/eurorack/marbles/random/distributions.h`
  (`BetaDistributionSample`, `FastBetaDistributionSample`, MIT) +
  `stmlib/dsp/dsp.h::Interpolate` (a 6-line lerp) + one or more inverse-CDF
  tables from `resources.cc`.
- Precedent: the déjà vu port for op seams; the `GR.*` Grids port for the
  "const-table trades flash" tradeoff.

---

## 0. The flash reality — measured from the *current* build

Parsed from `module/teletype.elf` (PT_LOAD LMAs), **this working tree, déjà vu
ops already linked** (`DV` / `DV.DV` / `DV.L` / `DV.R` confirmed present in the
image):

| Quantity | Value |
|----------|-------|
| Program image end (highest flash LMA) | `0x8005a7cc` |
| NVRAM region base (`__flash_nvram`) | `0x8005bc00` |
| **Free program flash** | **5,172 B ≈ 5.05 KB** |
| `.text` | 273,072 B |
| `.rodata` | 75,544 B |
| `.data` (flash init image) | ~12,956 B |

> Down from ~6.4 KB after the Grids commit — the déjà vu port (no tables) ate
> ~1.4 KB. NVRAM is a *separate* top-of-flash region; its ~7 KB margin **cannot**
> hold code or `const` tables. Everything below is measured against the 5.05 KB
> program-flash headroom.

### The decisive finding: the tables, not the code, are the cost

`BetaDistributionSample` is table-driven — a bilinear lookup over precomputed
inverse-CDF tables indexed by `(bias, spread)`. Measured from `resources.cc`:

| Table set | Tables | Floats each | Bytes | Verdict |
|---|---|---|---|---|
| `dist_icdf_4_3` only (`FastBetaDistributionSample`) | 1 | 389 | **1,556** | ✅ fits |
| `distributions_table` (full `BetaDistributionSample`) | 45 (5 bias × 9 spread) | 389 | **~68.5 KB** + 180 B ptr array | ❌ 13× over |

The 389 floats/table are 3 stacked inverse-CDF curves (main + a 5% and a 95%
tail table at higher resolution), `kIcdfTableSize = 128` → `129×3`. Soft-float
is already linked (déjà vu, `chaos.c`, `maths.c`) and `Interpolate` needs no
transcendentals, so **the code is ~free; the tables are everything.**

**Conclusion: the full bias+spread beta shaper is not portable to this hardware.**
It needs ~68.5 KB of `const` and there is 5.05 KB. Only the *fixed-shape* fast
path is viable.

---

## 1. Three options, ranked

### Option A — Fast beta, fixed shape (RECOMMENDED, ~2 KB)

Port `FastBetaDistributionSample` verbatim: one `dist_icdf_4_3` table
(beta(3,3), a symmetric bell with a fatter tail) + `Interpolate`. No `bias` /
`spread` controls — the distribution is fixed, exactly as Marbles uses it for
its clock-jitter path (`t_generator.cc:396`).

| Component | Bytes |
|---|---|
| `dist_icdf_4_3[389]` (`const float`) | 1,556 |
| `Interpolate` + `beta_next` wrapper | ~150 |
| Op(s) + struct + name + `match_token` + help | ~350 |
| **TOTAL** | **≈ 2.0–2.1 KB** — fits 5.05 KB, leaves ~3 KB |

Delivers the authentic Marbles X *character* (values cluster mid-range, tails
reach the rails) on any uniform input. This mirrors the déjà vu port's
philosophy: **port the reusable core, drop the hardware-specific breadth.**

### Option B — Bias/spread knobs via runtime approximation (no big table)

If the `spread`/`bias` knobs are genuinely wanted, skip the tables and compute a
parametric warp at op-eval time (ops run at metro rate — a few dozen float ops
is fine). E.g. a symmetric power/logit warp for spread + an affine skew for
bias, tuned to *approximate* the beta family. Flash ~0.5 KB, RAM 0. **Cost:** it
is no longer bit-faithful to Marbles and needs its own curve-fitting + tests.
Reasonable if the controls matter more than exactness.

### Option C — Reduced table subset (NOT recommended)

Keep e.g. 3 spread tables at fixed bias (`dist_icdf_4_0/4_4/4_8`) → 3 × 1,556 =
**4.7 KB**, which nearly exhausts the 5.05 KB headroom and leaves no room for
anything else on the branch. Gives a spread knob only, no bias. Poor
flash-per-feature; listed for completeness.

---

## 2. Op design (Option A)

The faithful pairing follows Marbles' own signal path (déjà vu → beta → scale):

| Op | Params | Returns | Semantics |
|---|---|---|---|
| `DV.B max` | 1 | 0..max | advance déjà vu one step, take its raw `[0,1)` uniform, warp through `FastBetaDistributionSample`, scale to `0..max` |
| `BETA x` | 1 | 0..16383 | *stateless* variant: treat `x` (0..16383) as the uniform, warp, return 0..16383 — composes with `RAND`, `TOSS`, patterns |

Recommend shipping **both**: `DV.B` is the "Marbles X output" one-liner; `BETA`
makes the shaper reusable on any source. `BETA` alone is the minimal add if you
want to keep the diff tiny.

```c
static float beta_fast(float uniform) {         // == FastBetaDistributionSample
    return tt_interpolate(dist_icdf_4_3, uniform, 128.0f);
}
```

---

## 3. File layout

- **`src/beta.c` / `.h`** — the `dist_icdf_4_3[389]` table, a private
  `tt_interpolate` (port of `stmlib::Interpolate`, 6 lines), and `beta_fast()`.
  Keeping the table in its own TU keeps the ~1.5 KB visible in the map and out of
  `maths.c`.
- **Ops** in the déjà vu block of `src/ops/maths.c` (next to `DV*`), or a
  dedicated `src/ops/beta.c`. Recommend `maths.c` to match déjà vu.
- **No new state, no NVRAM change, no `FIRSTRUN_KEY` bump** — `BETA` is pure;
  `DV.B` reuses the existing déjà vu global.

Register in the usual five places (per CLAUDE.md): `op.c`, `op_enum.h` (via
`python3 utils/op_enums.py`), `match_token.rl`, `module/config.mk`,
`tests/Makefile` + `simulator/Makefile`.

---

## 4. Porting the table

Copy `dist_icdf_4_3[]` out of `resources.cc` (lines ~4107+, 389 floats) into
`src/beta.c` as a `static const float`. Copy `Interpolate` from
`stmlib/dsp/dsp.h:43`. `MAKE_INTEGRAL_FRACTIONAL(x)` → two lines:
`int xi = (int)x; float xf = x - xi;`. That's the whole port — no namespaces, no
`resources` machinery, no `distributions_table` pointer array.

---

## 5. Testing (`tests/beta_tests.c`)

Beta is a pure function → far easier to test than déjà vu:

- **Range**: output always in `[0,1)` (→ `0..max` after scale) for inputs across
  `[0,1)`.
- **Monotonic**: `beta_fast` is a nondecreasing inverse-CDF — assert
  `beta_fast(a) <= beta_fast(b)` for `a < b` across a sweep.
- **Shape**: midpoint maps near 0.5 (`beta_fast(0.5) ≈ 0.5`, symmetric);
  histogram a uniform sweep and assert mass concentrates mid-range vs the
  ends (the "bell" property).
- **Float determinism**: assert on *structure* (range, monotonicity, symmetry),
  not exact bits — host hard-float vs AVR32 soft-float differ in the last ULP
  (same rule as the déjà vu/Grids tests).
- Registration self-check runs via `cd tests && make test`.
- **Shared-object gotcha**: `make clean` between host tests and the AVR32 build.
- On device: `DV.L 4`, `DV.DV 0`, `CV 1 V DV.B 5` in a metro → a locked 4-step
  loop of quantised voltages, now clustered toward the middle instead of flat.

---

## 6. Risks & notes

- **Flash is the only real constraint** and Option A clears it with ~3 KB to
  spare; Option C does not. Re-measure `teletype.elf` headroom before/after —
  the branch is already down to 5.05 KB.
- Full bias/spread is a **hardware non-starter** on UC3B0512 (68.5 KB tables).
  If it's a hard requirement, Option B (approximation) is the only path, at the
  cost of Marbles fidelity.
- `-fshort-enums -fno-common` when sizing any struct (see `MEMORY_USAGE.md`);
  irrelevant here since `BETA` adds no struct.

---

## 7. Effort

Small — smaller than déjà vu (pure function, no state, no seed-pinned RNG):
- `beta.c/.h` (table + interpolate + wrapper): ~0.5–1 h
- Ops + registration (`BETA` and/or `DV.B`): ~1 h
- Tests + help + on-device check: ~1 h

Flash ~2 KB (Option A). RAM 0. No mode, no OLED, no serialization.
