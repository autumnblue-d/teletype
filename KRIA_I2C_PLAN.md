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
