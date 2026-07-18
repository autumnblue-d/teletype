# Native arm64 AVR32 build image

On Apple Silicon, the stock `dewb/monome-build` image is `linux/amd64` and runs
under **QEMU emulation**, which makes firmware builds slow. This directory builds
a functionally identical toolchain as **native arm64** so builds run at full
speed with no emulation.

It is not a different toolchain: it compiles the exact same version-locked
sources dewb uses — **binutils 2.23.1, gcc 4.4.7 (Atmel `ddf7c8b`), newlib
1.19.0** — via the public
[`denravonska/avr32-toolchain`](https://github.com/denravonska/avr32-toolchain)
recipe, just on arm64 Ubuntu base images. The emitted `teletype.elf` is
functionally equivalent to a dewb build: verified byte-identical `.text`/`.data`
and identical `avr32-size` output when both build the same tree, so flash layout
and behavior are unchanged. A clean `cd module && make` takes on the order of
**~15 s** here versus minutes under QEMU. See [`Dockerfile`](Dockerfile) for
details, including the `config.guess`/`config.sub` refresh that lets the
2010-era sources configure on aarch64.

## Prerequisites

A **native arm64** Docker runtime. With [colima](https://github.com/abiosoft/colima):

```bash
colima start --vm-type vz --cpu 4 --memory 8   # native arm64 via Virtualization.Framework
docker info --format '{{.Architecture}}'       # must print aarch64
```

`--vm-type vz` (Apple's Virtualization.Framework) is what keeps execution native.
Do **not** pass `--platform linux/amd64` anywhere — that forces the slow QEMU path.

## Build the image (one-time, ~30–60 min)

```bash
# from the repo root
docker build --platform linux/arm64 -t teletype-avr32:arm64 toolchain/
```

The image builds the whole toolchain from source, so the first build is slow;
it is cached afterwards.

## Use it

```bash
# from the repo root
docker run --rm -v "$(pwd)":/target teletype-avr32:arm64 'cd module && make'

# interactive shell
docker run --rm -it -v "$(pwd)":/target --entrypoint bash teletype-avr32:arm64
```

Note there is **no** `--platform linux/amd64` — the container runs native arm64.

## Verifying equivalence to the emulated toolchain

From the same commit, build in both images and compare — sizes should match:

```bash
docker run --rm -v "$(pwd)":/target teletype-avr32:arm64 \
  'cd module && make clean >/dev/null && make >/dev/null && avr32-size teletype.elf'
docker run --rm --platform linux/amd64 -v "$(pwd)":/target dewb/monome-build \
  'cd module && make clean >/dev/null && make >/dev/null && avr32-size teletype.elf'
```

## Troubleshooting

- **`newlib` download 404** — sourceware may have moved the tarball again; update
  the `sed` newlib-URL rewrite in the `Dockerfile`.
- **`configure: unrecognized host aarch64...`** — the config-script refresh didn't
  reach a source tree; the retry loop should catch it, but if a package was added
  to the recipe, ensure it is extracted before the sweep.
- **Build too slow / OOM** — give the VM more resources: `colima stop && colima
  start --vm-type vz --cpu 6 --memory 10`.
