## MIDI out

MIDI out ops (`MO.*`) send MIDI messages to a class-compliant USB MIDI device — a synth
or any USB MIDI interface — plugged into the USB port (directly or behind a powered USB
hub). Unless your MIDI device is powered externally, make sure your power supply can
provide sufficient power! Please note that not all devices are supported.

`MO.CH` selects a module-level default channel (1..16) used by the ops without an explicit
channel parameter. This default is not saved per scene. The `#` variants (`MO.N#`, `MO.NO#`,
`MO.CC#`) take an explicit channel as their first argument and ignore the default.

Only a single USB MIDI device is addressable at a time, regardless of how many are connected
through a hub. Sending a message when no device is connected does nothing.

On a multi-port interface (e.g. the M-Audio MIDISPORT 2x2), `MO.PORT` selects which output
port / USB virtual cable the `MO.*` ops target: `0` = port A, `1` = port B. Like `MO.CH`,
the selected port is a module-level default and is not saved per scene.

### MO / I2M followers

Separately from the `MO.*` scripting ops, the native Kria/Meadowphysics engines can emit
MIDI automatically through two **followers** that mirror the running sequencer:

- **MO** — native USB MIDI out (same port as the `MO.*` ops)
- **I2M** — MIDI over i2c to an [i2c2midi](https://github.com/attejensen/i2c2midi) device

Followers are configured in an OLED editor reached from the **i2c view** of the native Kria
or Earthsea mode: select the **MO** or **I2M** entry on the grid to open its `MO EDIT` /
`I2M EDIT` screen. The editor is keyboard-driven: **up/down** move the cursor, **left/right**
change the focused value, and **enter** (or **1–4**) exits. The follower bank is global
(shared across scenes) and saved to flash.

#### Mode

The `MODE` field sets how sequencer tracks map to MIDI. Pitch modes route Kria's 4 tracks;
`8T` modes route 8 tracks (the Meadowphysics 8-track model):

- **PITCH SGL** — all enabled tracks play as pitched notes on one shared base channel
- **PITCH MUL** — track *n* plays pitched on channel *base + n* (one channel per track)
- **8T NOTES** — 8 tracks each fire a fixed note (`NOTE 1`–`NOTE 8`) on the base channel
- **8T CHANS** — all 8 tracks fire one shared note, each on its own channel (`CH 1`–`CH 8`)

#### Fields

Which fields appear depends on the mode:

- **ACTIVE** — enable / disable the follower (`ON` / `OFF`)
- **MODE** — one of the four modes above
- **OCT** — octave offset, `-3`…`+3` (pitch modes only)
- **CHAN** — base MIDI channel, `1`–`16` (all modes except `8T CHANS`)
- **NOTE** — the single shared fixed note (`8T CHANS` mode only)
- **PORT** — USB output port `A` / `B` (**MO** only)
- **TRK 1**…**TRK N** — per-track enable (`N` = 4 in pitch modes, 8 in `8T` modes)
- **NOTE 1**…**NOTE 8** — the fixed note per track slot (`8T NOTES` mode)
- **CH 1**…**CH 8** — the MIDI channel per track slot (`8T CHANS` mode)
