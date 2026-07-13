## Meadowphysics

Teletype includes a native **Meadowphysics** mode: the cascading-counter sequencer
from monome Ansible/Meadowphysics, ported to run inside Teletype and drive its own
CV/TR outputs. Eight independent counters each have a `count / speed / min / max`;
when a counter rolls over it fires its triggers/toggles, optionally re-syncs other
rows, and applies a rule (`inc / dec / max / min / rnd / pole / stop`) to a
destination row's count and/or speed.

`alt-M` shows/hides the Meadowphysics view; `space` (in the MP view)
plays/pauses the sequencer. The engine runs **independently of the view**, so
you can leave MP playing and switch to Edit to work on scripts. While playing,
Meadowphysics owns **only the CV/TR channels its voice mode uses** — 1 in `1V`,
2 in `2V`, all 4 in `4V`/`8T` — and script writes to those are suppressed. The
**remaining outputs stay free for scripts** (e.g. in `1V`, CV 2–4 and TR 2–4;
in `2V`, CV 3–4 and TR 3–4). Pausing releases everything back to scripts. It
runs on its own internal clock, or an external clock on trigger input 1.

### Keys

- `alt-M` — show / hide the MP view (the engine keeps running either way)
- `1` / `2` / `3` — Positions / Clock / Config views
- `space` — play / pause (in the MP view)
- `R` — reset (re-arm all counters)
- `V` — cycle voice mode (`1V` `2V` `4V` `8T` `SCR` — `SCR` fires script *n*+1 per row instead of driving CV/TR)
- `X` — toggle external clock (trigger input 1)
- `-` / `=` — tempo down / up
- `[` / `]` — scale (7 diatonic modes + chromatic); counter rows map to scale
  degrees, so pitch (1V/2V/4V) follows the selected scale

### Grid (16×8)

Eight rows are the eight counters; the sixteen columns are the range axis. Tap a
cell to set a counter's position; a second tap in the same row sets its range.
Hold column 0 for the speed / trigger view, column 1 for the rules view.

In the **Config view** (`3`) the grid becomes a scale editor over a global
16-slot bank: rows 6–7 (cols 0–7) select the slot, and the right half (cols
8–15, one row per scale degree) draws that degree's interval. Edits are saved to
flash and apply live. `[`/`]` also step the slot; slots 0–6 default to the
diatonic modes, 7 to chromatic, 8–15 are yours to edit.

### Outputs by voice mode

- **1V** — mono: CV 1 + TR 1
- **2V / 4V** — CV+TR pairs with oldest-note voice stealing
- **8T** — eight gates: rows 1–4 on TR 1–4, rows 5–8 as 0/10 V gates on CV 1–4

Meadowphysics state (counters, ranges, rules, scale, voice mode) is saved per
scene. Tempo/clock source are not currently stored per scene.

### Ops

The `MP.*` ops drive the native engine (they no longer control an external
Meadowphysics/Ansible over i2c):
