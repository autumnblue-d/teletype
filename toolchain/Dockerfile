# Native arm64 AVR32 build image for Teletype.
#
# This is an arm64 port of the dewb/monome-build recipe. The AVR32 toolchain is
# version-locked (GCC dropped AVR32; LLVM never had it), so we build the *same*
# sources dewb uses -- binutils 2.23.1, gcc 4.4.7 (Atmel), newlib 1.19.0 -- via
# the public denravonska/avr32-toolchain recipe, but on arm64 base images so the
# resulting toolchain runs natively (no QEMU) on Apple Silicon under colima's VZ
# runtime.
#
# The only arm64-specific wrinkle: the 2010-era config.guess/config.sub bundled
# in some of these old source packages predate aarch64, so configure would bail
# with "unrecognized host". We sweep-refresh every config.guess/config.sub with
# the host's current copies (autotools-dev) and drive the build in a
# refresh-and-retry loop that converges as each package is extracted.
#
# Build (from repo root, on a native arm64 Docker VM):
#   docker build --platform linux/arm64 -t teletype-avr32:arm64 toolchain/
# Use:
#   docker run --rm -v "$(pwd)":/target teletype-avr32:arm64 'cd module && make'

# ---------------------------------------------------------------------------
# Stage 1: build the AVR32 cross toolchain from source (native arm64).
# ---------------------------------------------------------------------------
FROM ubuntu:20.04 AS toolchain
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        autoconf automake autotools-dev libtool \
        texinfo bison flex gperf \
        libgmp-dev libmpfr-dev libmpc-dev libncurses-dev \
        curl wget ca-certificates git make patch \
        zip unzip bzip2 xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
RUN git clone --depth 1 https://github.com/denravonska/avr32-toolchain.git

WORKDIR /build/avr32-toolchain

# dewb's fix for the dead newlib FTP mirror. If sourceware has since moved the
# tarball too, this URL is the first thing to update.
RUN sed -i 's|ftp://sources.redhat.com/pub/newlib/|https://sourceware.org/pub/newlib/|' Makefile

# Build the toolchain into /avr32-tools.
#  1. Pre-extract every source tree so the first config-script sweep covers them.
#  2. Sweep config.guess/config.sub -> host's aarch64-aware copies.
#  3. Run install-cross in a retry loop, re-sweeping after each failure so
#     packages extracted late (in-tree gmp/mpfr/mpc, supp-tools) also get fixed.
RUN set -eux; \
    export PREFIX=/avr32-tools; \
    refresh() { \
        find /build/avr32-toolchain -type f -name config.sub \
            -exec cp -f /usr/share/misc/config.sub {} \; 2>/dev/null || true; \
        find /build/avr32-toolchain -type f -name config.guess \
            -exec cp -f /usr/share/misc/config.guess {} \; 2>/dev/null || true; \
    }; \
    make PREFIX="$PREFIX" \
        extract-autoconf extract-automake extract-texinfo extract-gperf \
        extract-avr32patches extract-headers extract-newlib \
        extract-binutils extract-gcc || true; \
    refresh; \
    ok=0; \
    for i in $(seq 1 20); do \
        if make PREFIX="$PREFIX" install-cross; then ok=1; break; fi; \
        echo "=== install-cross attempt $i failed; refreshing config scripts ==="; \
        refresh; \
    done; \
    [ "$ok" = 1 ]; \
    "$PREFIX"/bin/avr32-gcc --version

# ---------------------------------------------------------------------------
# Stage 2: slim runtime image with the toolchain + Teletype build utilities.
# ---------------------------------------------------------------------------
FROM ubuntu:22.04
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        make git ragel python3 zip bash ca-certificates \
        libmpfr6 libgmp10 libmpc3 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain /avr32-tools /avr32-tools
ENV PATH="/avr32-tools/bin:${PATH}"

WORKDIR /target
# Match dewb's calling convention: pass the build command as a single string,
# e.g.  docker run ... teletype-avr32:arm64 'cd module && make'
ENTRYPOINT ["bash", "-c"]
CMD ["cd module && make"]
