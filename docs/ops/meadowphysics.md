## Meadowphysics

Teletype includes a native **Meadowphysics** mode: the cascading-counter sequencer
from monome Ansible/Meadowphysics, ported to run inside Teletype and drive its own
CV/TR outputs. Eight independent counters each have a `count / speed / min / max`;
when a counter rolls over it fires its triggers/toggles, optionally re-syncs other
rows, and applies a rule (`inc / dec / max / min / rnd / pole / stop`) to a
destination row's count and/or speed.

Enter and leave the mode with `alt-M`. While active, Meadowphysics is the sole
writer of the four CV and four TR outputs (script output is suppressed), and it
runs on its own internal clock (or an external clock on trigger input 1).

### Keys

- `1` / `2` / `3` — Positions / Clock / Config views
- `space` — run / stop
- `R` — reset (re-arm all counters)
- `V` — cycle voice mode (`1V` `2V` `4V` `8T`)
- `X` — toggle external clock (trigger input 1)
- `-` / `=` — tempo down / up

### Grid (16×8)

Eight rows are the eight counters; the sixteen columns are the range axis. Tap a
cell to set a counter's position; a second tap in the same row sets its range.
Hold column 0 for the speed / trigger view, column 1 for the rules view.

### Outputs by voice mode

- **1V** — mono: CV 1 + TR 1
- **2V / 4V** — CV+TR pairs with oldest-note voice stealing
- **8T** — eight gates: rows 1–4 on TR 1–4, rows 5–8 as 0/10 V gates on CV 1–4

Meadowphysics state (counters, ranges, rules, scale, voice mode) is saved per
scene. Tempo/clock source are not currently stored per scene.

### Ops

The `MP.*` ops drive the native engine (they no longer control an external
Meadowphysics/Ansible over i2c):
