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
