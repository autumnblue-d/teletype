# Plan: i2c follower output for native Kria (+ Meadowphysics)

Add Ansible's **i2c leader** feature to telekria — let the on-board Kria (and MP)
engines route their per-track output over i2c to follower modules, not just the
4 CV/TR jacks. Ref: <https://monome.org/docs/ansible/i2c/>.

---

## STATUS — IMPLEMENTED for Kria ✅ (commits c41962d → 98cdce4, flashed)

All six followers and both Ansible ii pages are done and on hardware:

- **`src/kria_i2c.{c,h}`** — a faithful port of Ansible's `ansible_ii_leader.c`:
  a per-follower ops vtable (`init/mode/tr/mute/cv/octave/slew`) for **Just
  Friends, TELEXo, ER-301, Disting EX, W/syn, Crow**, driven over Teletype's i2c
  via `tele_ii_tx`. Logic ported near-verbatim (ET clamped;
  `outputs[t].semitones` → `kri2c_sem[t]`; `aux_param` → `kri2c_aux[t]`).
- **Dispatch** — `kria_i2c_cv/tr/slew` fan out to every follower that is `active`
  and whose `track_en` includes the track; **additive** to the CV/TR jacks. Hooked
  into the shell's `km_cv` / `km_tr` / `km_slew` output callbacks.
- **Persistence** — per-follower state lives in `kria_config_t.i2c[6]`
  (`{active, track_en, oct, mode}`), saved with the song; `FIRSTRUN_KEY` 0x27.
  Ansible defaults preserved (ER-301 gate/cv, W/syn oct −2).
- **UI (keyboard `4`)** — both Ansible pages:
  - *Toggle page* (`grid_KR_ii`): follower toggles at cols 5–6 (JF/TXo/ER301/
    Disting on col 5 rows 2–5; WSYN/Crow on col 6 rows 2–3). Tap = enable.
  - *Config page* (`grid_KR_ii_config`): **hold (5,7) + tap a follower** →
    octave (row 0, cols 0–6), operating mode (row 0, cols 12+), per-track routing
    (row 7, cols 0–3). Press (5,7) again to exit.

Cost: firmware +~4 KB total for i2c, ~29 KB headroom; tests 110/110.

**Caveat — hardware verification:** TELEXo + Just Friends are the confidently
testable pair. **ER-301, Disting EX, Crow, W/syn are ported verbatim from Ansible
but UNVERIFIED** (no modules on hand); the pitch/mode/gate byte sequences mirror
Ansible line-for-line but need on-bus confirmation.

**Meadowphysics i2c — also DONE** (commit `b7a9d3f`): MP's shell got the same
output-vtable fan-out (`mp_out_tr/cv/cv_gate` → the shared follower table), so MP
drives the same enabled followers as Kria. The follower table is **global**
(Ansible model) — configure it once in Kria's i2c view (key 4); MP shares it.
It loads when Kria is first entered or any `KR.*` op runs.

**KR.II op — DONE** (commit `9bd7973`): `KR.II f` / `KR.II f x` enables/disables
follower `f` (0-5) from scripts. **MP i2c view — DONE**: keyboard `4` in MP opens
the same shared i2c view as Kria. The follower bank is now a **global** nvram blob
(`nvram_data_t.kria_i2c[6]`), loaded at boot, shared by both engines
(`FIRSTRUN_KEY` 0x28).

**Still not done:** per-track routing via op (only enable is scriptable);
per-follower "operating mode" is stored/sent but its musical effect is only as
faithful as the Ansible port (untested for 4 of 6).

The original design notes below are kept for reference; the final implementation
followed them but adopted Ansible's full per-follower vtable rather than the
simpler two-follower sketch.

## What the Ansible feature is

Ansible is an i2c **leader** (it never acts as a follower). Its apps send
per-voice pitch/gate to follower devices. Supported followers:

| Follower | Role |
|----------|------|
| Just Friends (JF) | synth voice / function generator |
| TELEXo (TXo) | 4 enveloped oscillators or CV/gate pairs |
| ER-301 | gates + CV (many virtual outputs) |
| Disting EX | CV/gate pairs or CV→MIDI |
| W/Syn (WSYN) | synth voice |
| Crow | 2 ii CV/gate pairs |

Routing: a grid page enables followers; **bottom-left keys (the Kria track-select
keys) toggle which tracks drive which follower.** Default = all four tracks drive
all enabled followers.

## Why telekria is well-positioned

**Teletype is already an i2c leader.** We don't need a new i2c stack — everything
the followers need already exists in the tree:

- **Addresses** in `libavr32/src/ii.h`: `JF_ADDR 0x70`, `ER301_1 0x31…`,
  `WS_S_ADDR 0x76`, `CROW_ADDR_0 0x01…`, TELEXo + Disting addrs, etc.
- **Command formats + pitch conversions** in the existing op families
  `src/ops/{justfriends,telex,er301,disting,crow,wslash}.c` — the exact byte
  sequences to set a follower's note/gate are already written there.
- **The transport**: `tele_ii_tx(addr, data, len)` (used by every leader op).
- **The engine already emits per-track events** through the `kria_output_t`
  vtable (`tr` / `cv` (semitones) / `cv_slew`). The shell's output callbacks
  (`km_tr`/`km_cv`) are the single choke point where jack output happens today —
  i2c routing hooks in at exactly the same place.

So this is mostly **wiring existing output into existing i2c infrastructure**, not
new low-level work.

## Architecture

```
kria_engine  --vtable-->  km_cv(track, semitones)   -> stash last pitch[track]
                          km_tr(track, on)          -> (a) tele_tr / tele_cv  (jacks, today)
                                                       (b) kria_i2c_send(track, on, pitch)  <-- NEW
                                                              |
                                        per-track routing (cfg.i2c_route[track])
                                                              |
                          src/kria_i2c.c: for each enabled follower routed to
                          `track`, build the note/gate message (reusing the
                          ii.h addrs + the op command format) and tele_ii_tx().
```

Key point: the engine and grid are untouched. All new code lives in:
- **`src/kria_i2c.{c,h}`** — a follower-output helper: `kria_i2c_note(follower,
  voice, semitones, on)` that maps a semitone index to each follower's pitch unit
  and sends via `tele_ii_tx`. One small function per follower, lifted from the
  matching `src/ops/*.c`.
- **`module/kria_mode.c`** — extend `km_cv` to stash `last_pitch[track]`, and
  `km_tr` (gate high/low) to call `kria_i2c_send(track, on)` which fans out to the
  routed/enabled followers. Note-off/repeat already flow through `km_tr`, so gates
  and ratchets route for free.

### Per-follower output mapping (reuse the op logic)

Each follower needs a "set pitch + gate" translation from the engine's semitone
index (0–120):

| Follower | Pitch cmd (from op file) | Gate cmd | Voice/output mapping |
|----------|--------------------------|----------|----------------------|
| TELEXo | `TO.CV.N`/`TO.OSC` (14-bit, ET like TT) | `TO.TR` | track n → TXo out n |
| ER-301 | `ER301.CV.N` | `ER301.TR` | track n → 301 out n |
| Just Friends | `JF.NOTE`/`JF.VOX` (JF pitch unit) | (note w/ velocity) | track n → JF voice n |
| Disting EX | pitch/gate pair | gate | track n → pair n |
| W/Syn | `WS.PLAY` (pitch+trig) | — | track n → voice |
| Crow | `CROW` ii CV/gate | gate | track n → pair |

Pitch conversions differ per follower — TXo/ER-301 take a note number like TT’s
`N` op (ET-based, reuse `kria_note_to_cv`/the note form); JF/WSYN use their own
scaling (copy from `justfriends.c`/`wslash*.c`). The op files are the reference.

### Routing config (persisted)

Add to `kria_config_t` (Scenario-B budget has room; re-verify sizeof + bump
`FIRSTRUN_KEY`):

```c
uint8_t i2c_enable;                    // bitmask of enabled follower types (6 bits)
uint8_t i2c_route[KRIA_NUM_TRACKS];    // per-track bitmask: which followers it drives
```

Default: `i2c_enable = 0` (off — pure jack behavior unchanged), all-tracks-all
followers when enabled (matches Ansible).

## UI

Mirror Ansible's grid enable + per-track toggles. Add a **4th Kria view**
(keyboard `4`, alongside 1/2/3 seq/time/config — see the Time/Config views just
added):
- A row of follower enable toggles (JF/TXo/ER-301/Disting/WSYN/Crow).
- A track×follower area: the 4 track keys toggle that track's routing to the
  selected follower (Ansible's "bottom-left keys" gesture).

Optionally add TT ops later (e.g. `KR.II follower track on`) to script routing.

## Decisions to make (before coding)

1. **Additive or exclusive?** Route to followers *in addition to* the 4 jacks
   (recommended — expands voice count) or *instead of* per track? Suggest
   additive, with an optional per-track "mute jack" later.
2. **Which followers first?** Suggest **TELEXo + Just Friends** (most common,
   and TXo note maps like TT's `N`), then ER-301, then Crow/Disting/WSYN.
3. **Voice mapping.** Track n → follower output/voice n by default; revisit for
   ER-301 (many outs) and JF (6 voices) if you want spillover.
4. **Pitch/tuning.** Reuse each op's conversion so Kria-over-i2c tracks the same
   tuning as the equivalent TT op.

## Phases

- **A — Output plumbing (TELEXo + JF). ✅ DONE.** `src/kria_i2c.{c,h}`; `km_cv`
  fans out pitch, `km_tr` gate; additive to jacks. (Shipped as TXo+JF first, then
  extended.)
- **B — Routing config. ✅ DONE.** Per-follower state persisted in
  `kria_config_t.i2c[6]`; `FIRSTRUN_KEY` bumps; save/load via `flash_*_kria` +
  `kria_i2c_save/load`.
- **C — i2c view (UI). ✅ DONE.** Keyboard `4`: toggle page + per-follower config
  page (octave / mode / per-track), the hold-(5,7)+tap chord, rendered/handled in
  `kria_mode.c` like the Time/Config views.
- **D — Remaining followers. ✅ DONE.** ER-301, Disting EX, W/syn, Crow added by
  porting Ansible's `ansible_ii_leader.c` per-follower ops verbatim (untested for
  these four — see caveat).
- **E — MP + scripting. ✅ DONE.** MP i2c output (`b7a9d3f`) + MP i2c view (key 4)
  + `KR.II` enable op + global follower bank (`9bd7973`). ⬜ Remaining nicety:
  a per-track routing op (only follower enable is scriptable so far).

## Risks / watch-items

- **i2c throughput.** Kria fires per step, per track, per enabled follower, from
  the event-loop clock context. At fast tempos with several followers this is a
  lot of `tele_ii_tx` traffic — watch for bus saturation / clock jitter. The
  spike (Phase A) must check timing on hardware.
- **Pitch-unit correctness** per follower — the main source of "it plays but the
  notes are wrong." Lift conversions verbatim from the op files.
- **cfg layout change** → `FIRSTRUN_KEY` bump → NVRAM wipe on upgrade (back up).
- **Concurrent i2c**: if a TT script also drives i2c followers while Kria plays,
  both share the one leader bus — fine (single leader) but interleaving could
  surprise; document.
- **Follower addressing/units** vary by device (TXo channel offsets, ER-301
  virtual outputs, JF voice count) — confirm against each op file.

## Effort

Phase A (one follower, hardcoded routing) is small — a day-ish spike leveraging
existing code. B–D are incremental. The genuinely new risk is **i2c timing under
the sequencer clock**, which only hardware testing settles — so do Phase A first
and measure before building the config/UI.

---

## PLANNED: MO (USB MIDI out) + I2M (i2c2midi) as followers — OLED-configured

Two new note-based followers extend the table to 8 (`KR_F_I2M = 6`, `KR_F_MO = 7`;
`KRIA_I2C_FOLLOWERS` 6 -> 8). Both emit MIDI notes rather than CV/gate:
- **MO** — native USB MIDI out. No i2c: builds a 3-byte packet and calls the
  existing public seam `tele_midi_out(port, pack, 3)` (`teletype_io.h`). Carries a
  USB cable select (A=0 / B=1), mirroring the `MO.PORT` op.
- **I2M** — i2c2midi module at addr `I2C2MIDI = 0x3F`. Note-on `SEND_B3(20, ch,
  note, vel)`, note-off `SEND_B2(21, ch, note)` (see `src/ops/i2c2midi.c`).
  Channels 1..32 (device `MAX_CHANNEL`).

### Config UI: OLED + keyboard (NOT the grid)

MIDI config is parameter-rich and numeric (channel 1..32, cable, per-track notes,
per-gate channels) — a poor fit for a 16x8 LED grid. Instead, a shared module
unit **`module/kria_i2c_oled.{c,h}`** renders an 8-line `LABEL value` editor to
`region line[8]` (same substrate as `screen_refresh_kria`), navigated by the
keyboard: Up/Down move a field cursor, Left/Right (or -/=) change the value,
Enter/PAGE flips pages, a sub-cursor edits per-slot arrays. Called from BOTH the
Kria and MP i2c views (shared global bank).

- `src/kria_i2c.c` stays pure (data + i2c/MIDI emit + accessors). Screen access is
  module-layer only. New accessors: `set/get_channel`, `set/get_port`,
  `set/get_note(slot)`, `set/get_chan_slot(slot)`, mode/track getters.
- The **grid** i2c view keeps only the 8 follower TOGGLE cells (activate / show
  active). Selecting MO/I2M opens the OLED editor; **CV followers keep their grid
  config pages** (hybrid: grid for CV, OLED+keyboard for MIDI). Minor idiom
  inconsistency, acceptable; CV pages could migrate to OLED later for uniformity.

### MIDI follower modes (mode_ct = 4)

Pick the mode to match how the follower is driven:

| mode | driven by | note source | channel mapping |
|------|-----------|-------------|-----------------|
| 0 PITCH.SINGLE | Kria, MP 1V/2V/4V | sequencer pitch (`kri2c_sem`) | all routed tracks -> base `chan` |
| 1 PITCH.MULTI  | Kria, MP 1V/2V/4V | sequencer pitch | track n -> `chan + n` (clamped 16/32) |
| 2 8T.NOTES     | MP 8T | 8 fixed `notes[0..7]` (GM drum defaults) | all on base `chan` |
| 3 8T.CHANS     | MP 8T | one fixed note (`notes[0]`) | 8 selectable `chans[0..7]`, one per gate |

Note math (pitched): `note = kri2c_sem[track] + 12*(base_oct + oct)`, clamp 0..127
(same as the Disting-EX MIDI modes already in kria_i2c.c). Velocity: fixed default
(configurable "fixed / from duration" via `aux_to_midi_vel`).

### 8T (8 gates, no pitch)

`MP_8T` = rows 0-3 -> TR, rows 4-7 -> CV-as-gate; today both call
`kria_i2c_tr(ch, on)` with ch 0-3, so rows 4-7 ALIAS rows 0-3. To get 8 distinct
gates: the MP binding offsets the CV-gate rows to follower **tracks 4-7**
(`mp_out_cv_gate` -> `kria_i2c_tr(ch + 4, on)`; VERIFY the engine's `ch` arg), and
`track_en` widens to an **8-bit** mask. Kria uses only 0-3. 8T is gate-only so
`kri2c_sem` is untouched; the note comes from `notes[]` (mode 2) or the single
fixed note (mode 3). GM drum defaults for `notes[8]`, e.g.
{36 kick, 38 snare, 42 CH, 46 OH, 39 clap, 45 low tom, 49 crash, 51 ride}
(finalize at impl).

### Data model / persistence

Add to `i2c_follower_t` (runtime) and `kria_i2c_fstate_t` (persist):
`chan` (base MIDI ch), `port` (MO cable A/B), `notes[8]` (GM defaults),
`chans[8]` (8T per-gate channels); widen `track_en` to 8-bit. New `i2c_ops_t`
descriptors: `midi` flag (drives OLED editor + mode set), `chan_max` (16 MO / 32
I2M). `FIRSTRUN_KEY` 0x28 -> 0x29. The erased-blob sanitize in `kria_i2c_load`
switches from the `track_en > 0x0f` test to the `active > 1` sentinel (track_en
can now legitimately be 0xff). NVRAM grows ~150 B (2 followers x ~19 B extra);
fits 145K easily. RAM `followers[]` +~150 B — negligible.

### KR/MP channel semantics (recap)

One shared global bank -> MO/I2M `chan`/`mode` are identical whether Kria or MP is
playing that follower. For independent KR vs MP channels, route KR to one MIDI
follower and MP to the other. Kria = 4 pitched tracks; MP 1V/2V/4V = its owned
CV voices (pitched); MP 8T = 8 gates (modes 2/3).

### Phasing — IMPLEMENTED (built + 111/111 tests; not yet flashed)

1. Data model: followers 6->8, add fields, `chan_max`/`midi` ops descriptors,
   FIRSTRUN bump + sanitize fix, NVRAM re-verify.
2. MO vtable (via `tele_midi_out`, cable-aware) + table row. **Hardware-testable
   now** (USB MIDI -> DAW/synth) — do first.
3. I2M vtable (0x3F note-on/off) + table row. (i2c2midi hardware unverified.)
4. `kria_i2c_oled` unit: render + keyboard nav + pages; wire into KR + MP i2c
   views; new accessors in kria_i2c.c.
5. 8T: MP binding track 4-7 offset + 8-bit track_en; modes 2/3 emit paths.
6. Build + tests (note math, channel/mode mapping, 8T routing) + flash.

### Status — SHIPPED (commit 201b1e7 on kria, pushed to local; flashed 2026-07-04)

All 6 phases done: built, 111/111 tests, on hardware. Firmware text ->0x5332a,
nvram 144,980 fits 145K. Verified during impl: MP `cv_gate` passes ch 0-3 (from
`out_cv_gate(e, n-4, ..)`), so the +4 offset in `mp_out_cv_gate` yields distinct
follower gates 4-7. 8T mode 3 confirmed = 1 fixed note / 8 selectable channels.

**Editor exit-key gotcha (fixed):** `process_global_keys` (main.c) consumes ESC
(->preset) and TAB (->mode switch) *before* the mode handler, so the OLED editor
never receives them. The first cut used ESC/TAB to exit -> got stuck. Fix: the
editor consumes only the arrows + ENTER (explicit exit) and returns 0 for
anything else, so the shell falls through -> a view key (1/2/3/4) leaves the
editor AND switches view. Also `kria/meadowphysics_mode_exit` call
`kria_i2c_oled_exit()` so the editor never persists across a mode change.

### Open / watch

- Note-off recomputes note from state (mono-per-track stable); mask/pitch change
  mid-gate could hang a note -> optional `last_note[track]` per MIDI follower.
- I2M + 8T are hardware-unverified (no i2c2midi module on hand); MO is testable
  over USB MIDI. Velocity is fixed 100 (velocity-from-duration deferred).
- No unit tests on the emit path (i2c layer has none; needs seam-capture infra).
