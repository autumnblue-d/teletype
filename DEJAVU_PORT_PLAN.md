# Plan: Marbles déjà vu random ops in Teletype

Implementation plan for porting Mutable Instruments **Marbles**' *déjà vu*
random-sequence engine into Teletype as a small set of scripting ops. Unlike
the stateless `GR.*` / `DR.*` drum ops, déjà vu is inherently **stateful** — the
loop buffer *is* the feature — so this port carries a small block of runtime
state.

- Source: `~/git/eurorack/marbles` (Mutable Instruments, STM32/C++). The entire
  reusable core is one header-only class: `random/random_sequence.h`
  (`RandomSequence`, MIT). Everything else (t/x-y generators, quantizer,
  ramp, UI, CV reader) is out of scope.
- Target: this repo, branch `kria` (or a new `dejavu` branch off it), on top of
  the shipped `GR.*` Grids ops.
- Precedent: the `GR.*` port (`GRIDS_PORT_PLAN.md`) for the op-registration
  seams; the RNG-using ops in `src/ops/maths.c` (`RAND`/`TOSS`, which pull
  `random_next(&ss->rand_states...)`) for the randomness source.

---

## 0. The flash / RAM reality — measured from the current build

From `module/teletype.elf` **after the Grids commit** (`9f9236b`):

| Quantity | Value |
|----------|-------|
| Program image end (LMA of `.flash_nvram`) | `0x8005a260` |
| NVRAM region base (`__flash_nvram_size__ = 145K`) | `0x8005bc00` |
| **Free program flash** | **~6.4 KB** |
| `sizeof(nvram_data_t)` (`.flash_nvram`) | 140,980 B (unchanged) |
| Free NVRAM margin | ~7.3 KB (≈ 1 scene slot) |

> ⚠️ Measure struct sizes with `-fshort-enums -fno-common` (see `MEMORY_USAGE.md`).

### The decisive finding: soft-float is already linked

Marbles' déjà vu is float-native (loop values, `p = (2·dv−1)²`, `GetFloat`).
AVR32 UC3B has **no FPU**, which normally means dragging in the multi-KB
soft-float runtime. **But Teletype already uses float** (`src/chaos.c`,
`src/ops/maths.c`'s `logf`, `src/es_engine.c`…), so the AVR32 helpers
`__avr32_f32_mul` / `_sub` / `_div` and int↔float conversions are **already in
flash**. déjà vu needs no transcendentals (no `sqrt`/`log`). So:

- A **faithful float port** is both cheapest and safest — no fixed-point rewrite,
  ~0 marginal soft-float cost.

### Size estimate

| Component | Bytes | Basis |
|---|---|---|
| `NextValue` + `Init` + setters (float, ~40 LoC) | ~800–1,200 | est. `-Os` float-branchy code |
| Op wrappers (`DV` / `DV.DV` / `DV.L` / `DV.R`) + structs + names | ~350 | cf. `op_GR_*` |
| `tele_ops[]` ptrs + `match_token` + help (4 ops) | ~600 | |
| Soft-float runtime | **~0** | already linked |
| **TOTAL flash** | **≈ 1.8–2.2 KB** | fits the ~6.4 KB headroom |
| **RAM (state)** | **~170 B / channel** | see §3 |

No const data tables (contrast Grids' 2.4 KB map): déjà vu trades flash for a
little RAM.

---

## 1. Op design

déjà vu is a *set-params-then-read* engine, like a tiny sequencer. Single global
instance in v1 (multi-channel deferred, §6):

| Op | Params | Returns | Semantics |
|---|---|---|---|
| `DV max` | 1 | 0..max | advance the sequence one step; return the value scaled to `0..max` |
| `DV.DV x` | 1 (set) | — | set déjà-vu amount, `x` = 0..100 → 0.0..1.0 (0 = fresh random; 50 = locked loop; 100 = shuffled loop) |
| `DV.L n` | 1 (set) | — | set loop length, `n` = 1..16 |
| `DV.R` | 0 | — | `Record()` — mark the current position as the loop start (re-lock a fresh loop) |

Notes:
- `DV.DV` / `DV.L` are set-ops (no return), matching Teletype variable-op style;
  optionally also make them get/set (read back current value) via
  `MAKE_GET_SET_OP`.
- Output scaling: `NextValue` returns `[0,1)`; map with `(int)(v * (max + 1))`,
  clamped. Feeding this into `CV`/`N`/pattern ops is the intended use.
- Determinism: seed déjà vu's RNG from Teletype's existing `SEED`/scene seed so
  results are reproducible (see §3, §7).

---

## 2. File layout

Header-only source ports to a small C module (déjà vu is one class; flatten the
C++ to plain C structs + functions):

- **`src/dejavu.c` / `.h`** — a `dejavu_t` struct (the ex-`RandomSequence`
  fields) + functions:
  ```c
  typedef struct {
      float loop[16];          // kDejaVuBufferSize
      float history[16];       // kHistoryBufferSize (drop if replay unused, §5)
      int   loop_write_head, length, step;
      int   record_head, replay_head, replay_start;
      uint32_t replay_hash, replay_shift;
      float deja_vu;
      random_state_t *rng;     // Teletype RNG, replaces RandomStream
  } dejavu_t;

  void  dejavu_init(dejavu_t*, random_state_t *rng);
  float dejavu_next(dejavu_t*, bool deterministic, float value);  // NextValue
  void  dejavu_record(dejavu_t*);
  void  dejavu_set_deja_vu(dejavu_t*, float);
  void  dejavu_set_length(dejavu_t*, int);
  ```
- **Ops** in a new block in `src/ops/maths.c` next to `GR.*` (keeps the diff
  small, no new op TU), or a dedicated `src/ops/dejavu.c`. Recommend `maths.c`.
- **State instance**: a single global `dejavu_t` (see §3). No new NVRAM field.

Constants: `kDejaVuBufferSize = kHistoryBufferSize = 16`,
`kMaxUint32 = 4294967296.0f`.

---

## 3. State: where it lives (the one real decision)

déjà vu needs ~170 B of persistent state (`loop[16]`+`history[16]` floats +
heads). Two options:

- **A — Global static** (recommended for v1): one `dejavu_t` in `maths.c` (or a
  small `src/dejavu.c` owning it), initialised at boot from a dedicated
  `random_state_t`. Like the Kria/ES global banks. **No `scene_state` change, no
  NVRAM format change, no `FIRSTRUN_KEY` bump** — existing scenes survive. State
  is *not* per-scene and resets on power cycle.
- **B — Per-scene**: put `dejavu_t` (or a serializable subset) in
  `scene_state_t`. Survives scene switches and can persist to flash, but forces
  an NVRAM layout change → `FIRSTRUN_KEY` bump + `scene_serialization.c` work +
  RAM growth in the single `scene_state` (watch the SRAM budget — floats make it
  chunky). Heavier; defer unless per-scene déjà vu is explicitly wanted.

RNG source: reuse a `random_state_t`. Cleanest is to add one to
`scene_rand_t` (the `RAND_STATES_COUNT` union in `state.h`) *or* keep a private
static `random_state_t` in `dejavu.c` seeded at init — the latter avoids the
scene_state change entirely, consistent with option A.

Substitution for Marbles' `RandomStream::GetFloat()`:
```c
static float dv_rand_float(dejavu_t *d) {
    return (float)random_next(d->rng) / 4294967296.0f;  // [0,1)
}
```
This drops Marbles' 128-entry `RingBuffer` (512 B) and `RandomGenerator`
fallback entirely — those exist only for Marbles' external-clock entropy mixing,
irrelevant on Teletype.

---

## 4. Algorithm port (verbatim, de-C++'d)

`NextValue` is copied 1:1 from `random_sequence.h:161-207`, with
`random_stream_->GetFloat()` → `dv_rand_float(d)` and `this->` fields → `d->`.
The core (dropping the replay branch, see §5):

```c
float dejavu_next(dejavu_t *d, bool deterministic, float value) {
    const float p_sqrt = 2.0f * d->deja_vu - 1.0f;
    const float p = p_sqrt * p_sqrt;
    const bool mutate = dv_rand_float(d) < p;

    if (mutate && d->deja_vu <= 0.5f) {              // regenerate a slot
        d->loop[d->loop_write_head] =
            deterministic ? 1.0f + value : dv_rand_float(d);
        d->loop_write_head = (d->loop_write_head + 1) % 16;
        d->step = d->length - 1;
    } else if (mutate) {                              // deja_vu > 0.5: jump
        d->step = (int)(dv_rand_float(d) * (float)d->length);
    } else {                                          // replay in order
        d->step = d->step + 1;
        if (d->step >= d->length) d->step = 0;
    }
    uint32_t i = d->loop_write_head + 16 - d->length + d->step;
    float result = d->loop[i % 16];
    if (result >= 1.0f) result -= 1.0f;
    else if (deterministic) result = 0.5f;
    return result;
}
```

`set_length` clamps to 1..16; `set_deja_vu` stores the float; `Init` fills
`loop[]` from the RNG and zeroes heads. Keep the semantics **exactly** — the
`loop_write_head + 16 - length + step` indexing and the `>= 1.0f` tagging are
the subtle parts (mirror Grids' "copy the ordering literally" rule).

---

## 5. Simplification: drop the replay / rewrite path

Marbles uses `history_[]`, `Replay*()`, `GetReplayValue()`, `RewriteValue()`
only for its ASR / quantizer / external-acquisition modes — none of which exist
on Teletype. Dropping them removes:
- `history_[16]` (−64 B RAM), `replay_head/start/hash/shift`, the `redo_*`
  pointers, and ~150 B of code.

v1 keeps only `loop[]` + `length`/`step`/`deja_vu`/`loop_write_head`. If a future
ASR-style op wants it, restore from the original header.

---

## 6. Optional extensions (deferred)

- **Multi-channel** `DV ch max` (Marbles has independent t and x/y sequences):
  make the global an array `dejavu_t dv[N]`; +~170 B RAM per channel. Recommend
  N = 2–4 if wanted.
- **`DV.SEED x`** to reseed a channel for reproducible loops.
- **Per-scene persistence** (option B in §3).
- **Quantized / distribution output** (Marbles' `discrete_distribution_*`) — a
  separate, larger feature; not part of déjà vu proper.

---

## 7. Testing

Stateful + float + RNG makes this trickier to unit-test than Grids. Strategy:

- **`tests/dejavu_tests.c`** (new; register in `tests/Makefile` + `main.c` like
  `grids_helpers_tests.o`) with a **fixed-seeded** `random_state_t` so the
  sequence is deterministic:
  - `deja_vu = 0.0` → the sequence must **repeat with period `length`** (lock a
    loop, read `2*length` values, assert second half == first half).
  - `deja_vu = 0.5` → values change (assert not all equal; can't assert exact
    without pinning the RNG stream — pin it and snapshot expected values as an
    oracle, same technique as the Grids test).
  - `set_length` clamping (0 and 17 are rejected), output range `[0,max]`.
- `cd tests && make test` — runs the `op_mod_suite` registration self-check
  (every op in `tele_ops` needs an enum + token entry).
- **Shared-object gotcha:** `make clean` between host tests and the AVR32 build
  (host and target share `src/*.o`).
- On device: `DV.L 4`, `DV.DV 50`, then `DV 16383` inside a metro — output should
  lock to a repeating 4-step loop; nudging `DV.DV` away from 50 should start
  mutating it.

---

## 8. Risks & notes

- **Float determinism across host/target**: the AVR32 soft-float and host
  hardware float can differ in the last ULP. Keep test asserts on *structure*
  (periodicity, range, equality of replayed slots) rather than exact float bit
  patterns; use integer snapshots only for a fixed, pinned RNG stream and allow
  the mapping `(int)(v*(max+1))` to absorb tiny float diffs.
- **Indexing subtlety**: `loop_write_head + 16 - length + step` must stay `% 16`
  with the `+16` to avoid negative modulo — port literally.
- **No flash-format change** with option A → no `FIRSTRUN_KEY` bump.
- **SRAM**: +~170 B global is trivial against the ~64 KB budget; option B (per
  scene) is the only variant that pressures the tight `scene_state`.

---

## 9. Effort

Small–medium — a bit more than Grids because it's stateful and needs
seed-pinned tests:
- `dejavu.c/.h` (flatten the class, drop replay): ~1–2 h
- Ops + registration (4 ops): ~1 h
- Seed-pinned tests + help + on-device check: ~1–2 h

No mode, no OLED, no serialization (option A). Flash ~2 KB, RAM ~170 B.
