# Building Teletype with USB-hub + dock support

Two ways to get the firmware. Both use the `hub-snapshot` libavr32 branch.

> **Heads up — reseeds scenes.** This firmware shrinks the NVRAM region
> (200K→199K) for the hub driver, moving its base. The **first boot after
> flashing wipes saved scenes.** Back them up (WRITE TO USB) on your current
> firmware first.

---

## Path A — vanilla teletype + patch

For stock `monome/teletype`. Wires in the hub driver via one patch.

```sh
git clone --recursive https://github.com/monome/teletype
cd teletype

# put libavr32 on the hub branch
cd libavr32
git remote add hub https://github.com/autumnblue-d/libavr32
git fetch hub hub-snapshot && git checkout hub-snapshot
cd ..

# apply the teletype-side wiring (module/* only)
git apply libavr32/libavr32-teletype-hub.patch
```

## Path B — the full branch (no patch)

Everything already committed (hub/dock + Kria/MP/Earthsea + MIDI-out). Nothing to apply.

```sh
git clone --recursive -b trilogy https://github.com/autumnblue-d/teletype
cd teletype
```

---

## Build (either path)

Needs [Ragel](http://www.colm.net/open-source/ragel/) and an AVR32 toolchain in Docker.

**Apple Silicon (native, fast):**
```sh
docker build --platform linux/arm64 -t teletype-avr32:arm64 toolchain/
docker run --rm -v "$(pwd)":/target teletype-avr32:arm64 'cd module && make'
```

**Anything else (emulated fallback):**
```sh
docker run --rm --platform linux/amd64 -v "$(pwd)":/target dewb/monome-build 'cd module && make'
```

Output: `module/teletype.{hex,bin,elf}` → flash with `module/flash.sh` (or your usual DFU flow).

---

Hub compatibility notes and the trace legend: `USB_DOCK_NOTES.md`. Patch details: `libavr32/TELETYPE_HUB_PATCH.md`.
