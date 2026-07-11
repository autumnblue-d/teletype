# Native apps: Kria, Meadowphysics & Earthsea

This firmware ports three monome Ansible apps to run **natively inside
Teletype**: the **Kria** step sequencer, the **Meadowphysics** cascading-counter
sequencer, and the **Earthsea** gestural pattern player. Each has its own grid
UI, drives Teletype's CV/TR outputs directly, can send i2c to followers (Just
Friends, TELEXo, ER-301, Disting EX, W/, Crow), and shares a global editable
scale bank. Each is also scriptable through its own op family (`KR.*`, `MP.*`,
`ES.*`).

The engines keep running **in the background** after you leave their view, so
you can start a sequence, switch to Edit to work on scripts, and it keeps
playing.

## Entering & leaving a mode

Switch modes from the USB keyboard with `Alt` + a letter. Each combo is a
**toggle** — press it again to return to the previous mode.

| Key      | Mode                     |
| -------- | ------------------------ |
| `Alt-K`  | Kria                     |
| `Alt-M`  | Meadowphysics            |
| `Alt-E`  | Earthsea                 |

Note that the `Tab` cycle (Live → Edit → Pattern) does **not** reach these
apps — `Alt` + letter is the only way in. `Alt-?` / `Alt-H` opens the on-device
help, whose last three pages document these apps.

## Shared conventions

These behaviours are common to all three apps.

### Saving (`S`)

Press `S` to save the current app's state; a `SAVED` banner confirms it
briefly. Kria and Earthsea save their state **per scene**. Meadowphysics saves
to a **global 8-slot preset bank** (`S` writes the selected slot, `L` loads it).

Saving a Teletype scene the usual way (preset-write mode, `Alt-Enter`) also
flushes any dirty app state to flash, so you don't have to save each app
separately before writing a scene.

### The global scale bank

Kria, Meadowphysics and Earthsea share one **16-slot** scale bank, each slot
holding an 8-step interval set. Slots 0–6 default to the seven diatonic modes,
slot 7 is chromatic, and slots 8–15 are yours to edit. Edits are written to
flash and take effect live. Reach the editor through each app's config/scale
view (see below); `[` / `]` step between slots.

### Output ownership

While an app is playing it only claims the CV/TR outputs its current voice mode
actually uses; **every other output stays free for your scripts**. Pausing or
disengaging the app releases all outputs back to scripts. The per-app sections
below list exactly which jacks each voice mode owns.

### Configuring i2c followers (`4`)

All three apps share one i2c follower bank, opened with key `4` from any of
them. Here you enable followers and route the app's tracks/voices to them; the
bank is global and saved to flash. Followers echo the app's output additively
(they don't replace the CV/TR jacks).

**Grid:** two columns hold the eight followers — **column 5, rows 2–5** = Just
Friends, TELEXo, ER-301, Disting EX; **column 6, rows 2–5** = W/syn, Crow,
i2c2midi, USB MIDI Out. Tap a follower cell to toggle it on / off.

To configure one, tap cell **(column 5, row 7)** to arm the config modifier,
then tap a follower. CV followers open a grid config page; the MIDI followers
(i2c2midi / USB MIDI Out) open an OLED editor for channel/port. On a CV
follower's config page:

- **row 0, cols 0–6** — octave offset (centre = 0)
- **row 0, cols 12+** — the follower's operating mode (when it has more than one)
- **row 7, cols 0–3** — enable each track/voice that feeds this follower

Tap **(column 5, row 7)** again to leave the config page. From scripts, `KR.II`
toggles follower output for the Kria and Meadowphysics engines.

## Kria

Kria is a four-track step sequencer. Each track has independent trigger, note,
octave, duration, repeat, glide, loop, timing and probability pages.

**Keys:** `1` / `2` / `3` select views; `4` opens the i2c follower view; `S`
saves (per scene).

**Grid (16×8):** the bottom row (row 7) is the transport / page selector; rows
0–6 render the currently selected page.

- **x0–x3** — select the active track (1 of 4). Hold the LOOP column and tap
  x0–x3 to **mute** a track.
- **x5** TRIG / RPT · **x6** NOTE · **x7** OCT / GLIDE · **x8** DUR
- **x10** LOOP · **x11** TIME (per-step clock divide) · **x12** PROB
- **x14** SCALE · **x15** PATTERN (hold for CUE)

The **SCALE page** (x14) edits the current slot of the global scale bank. The
**Config view** (`3`) holds the note-sync / loop-sync / tie / meta-reset
toggles.

**Outputs:** track *n* drives **CV *n*** and **TR *n***. A muted track frees its
CV/TR for scripts. State is saved per scene.

## Meadowphysics

Meadowphysics is eight cascading counters. Each counter has a count, speed, min
and max; when it rolls over it fires its outputs, can re-sync other rows, and
applies a rule (increment, decrement, max, min, random, pole, stop) to a target
row.

**Keys:**

| Key       | Action                                             |
| --------- | -------------------------------------------------- |
| `1`/`2`/`3` | Positions / Clock / Config views                 |
| `4`       | i2c follower view                                  |
| `5`       | preset browser                                     |
| `space`   | play / pause                                       |
| `R`       | reset (re-arm all counters)                        |
| `V`       | cycle voice mode (`1V` `2V` `4V` `8T` `SCR`)       |
| `X`       | toggle external clock (trigger input 1)            |
| `-` / `=` | tempo down / up                                    |
| `[` / `]` | previous / next scale slot                         |
| `S` / `L` | save / load the selected preset slot               |

**Grid (16×8):** eight rows are the eight counters, sixteen columns are the
count/range axis. Tap a cell to set a counter's position; a second tap in the
same row sets its range. Hold **column 0** for the speed/trigger view, **column
1** for the rules view. The **Config view** (`3`) turns the grid into the scale
editor.

The **Preset browser** (`5`) manages the 8-slot global preset bank. Column 0
(rows 0–7) selects a slot — double-tap it to load and close the browser — while
the right 8×8 block (cols 8–15) is a drawable **glyph** canvas that identifies
the working config. `S` saves the working config and glyph to the selected slot
(a `SAVE n` banner confirms); `L` loads from it (`LOAD n`).

**Voice modes / outputs:**

- **1V** — mono: CV 1 + TR 1
- **2V / 4V** — CV+TR pairs with oldest-note voice stealing
- **8T** — eight gates: rows 1–4 on TR 1–4, rows 5–8 as 0/10 V gates on CV 1–4
- **SCR** — owns no jacks; each row *n* fires script *n*+1

Meadowphysics uses the **global preset bank**, not per-scene storage.

## Earthsea

Earthsea plays and records gestural note patterns across a fourths-layout grid
keyboard.

**Keys:** `[` / `]` select the previous / next pattern; `4` opens the i2c
follower view; `S` saves (per scene).

**Grid (16×8):** column 0 is the transport / function strip; columns 1–15 are
the note keyboard.

- **Y0** start / stop
- **Y1** hold = peek at patterns (and scale view); tap = lock/close
- **Y2** hold = arm recording
- **Y3** toggle loop · **Y4** toggle arp
- **Y5** hold = EDGE overlay · **Y6** hold = RUNES overlay · **Y7** hold =
  VOICES overlay

On the keyboard, **col 15 / row 0** is the REST key; row 0 also scrubs playback
while a pattern is playing.

**Edge modes** determine how held notes behave: pattern, drone, and fixed
(select via the EDGE overlay or `ES.MODE`).

**Outputs:** up to four voices, voice *n* → CV *n* + TR *n* gate; Earthsea only
claims those jacks while engaged. State is saved per scene.

## Scripting the apps

Each app has an op family in the reference below. This guide covers hands-on
grid use; the op tables give the full prototypes and value ranges.

- **`KR.*`** — drive the native Kria engine from scripts (documented in the
  **Ansible** section, since Kria originates there): `KR.RUN`, `KR.PAT`,
  `KR.SCALE`, `KR.PERIOD`, `KR.POS`, `KR.CUE`, `KR.MUTE` / `KR.TMUTE`, `KR.CLK`,
  `KR.PG`, `KR.DIR`, `KR.DUR`, `KR.CV`, `KR.II`, and more.
- **`MP.*`** — drive the native Meadowphysics engine (see the **Meadowphysics**
  section): `MP.RUN`, `MP.STOP`, `MP.RESET`, `MP.SYNC`, `MP.CLK`, `MP.VOICE`,
  `MP.PERIOD`, `MP.SCALE`, `MP.SCL`, `MP.PRESET`.
- **`ES.*`** — drive the native Earthsea engine (see the **Earthsea** section):
  `ES.RUN`, `ES.MODE`, `ES.CLOCK`, `ES.RESET`, `ES.PATTERN`, `ES.TRANS`,
  `ES.STOP`, `ES.CV`, `ES.MAGIC`. (`ES.PRESET` and `ES.TRIPLE` instead target an
  external Earthsea/Ansible over i2c.)

`KR.II` enables or disables i2c follower output for the native Kria and
Meadowphysics engines.
