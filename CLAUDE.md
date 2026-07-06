# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Teletype is firmware for a monome Eurorack module. The codebase contains the core algorithm/command interpreter, hardware-specific module code, tests, and documentation generation.

## Build & Development Commands

### Prerequisites
- **Ragel** (state machine compiler): Required for building. Version 6.9 is known to work.
  ```bash
  brew install ragel  # macOS
  apt install ragel   # Debian/Ubuntu
  ```
- **For module firmware**: AVR32 toolchain (use Docker image `dewb/monome-build` to avoid setup)
  - On Apple Silicon (arm64): the image is `linux/amd64`, so add `--platform linux/amd64` to every `docker run` (it silently exits otherwise), and pass non-interactive commands as a single string since the entrypoint is `bash -c`. Example: `docker run --rm --platform linux/amd64 -v "$(pwd)":/target dewb/monome-build 'cd module && make'`. Colima (`brew install colima docker && colima start`) supplies the emulation. See README for details.
- **For documentation**: Python 3.6+, Pandoc, TexLive/TinyTeX (see README.md for details)

### Common Commands

```bash
# Tests (run from project root)
cd tests
make test           # Run tests with colored output via greatest/greenest
make test-travis    # Run tests (CI format, plain output)

# Code formatting
make format         # Format only uncommitted changes (changed files)
make format-all     # Format all .c and .h files in the repo

# Module firmware
cd module
make clean
make                # Build teletype.elf

# Release package
make release        # Creates teletype.zip with firmware, docs, and scripts
```

### Running Individual Tests

Tests are organized in `tests/`:
- `main.c` - Test entry point
- `*_tests.c` files - Individual test suites
- Uses "greatest" test framework (in `tests/greatest/`)

To debug or run specific test logic, examine the relevant `*_tests.c` file and rebuild with `make test`.

## Code Architecture

### Directory Structure

- **`src/`** - Core algorithm and command interpreter
  - `state.c/h` - State management (variables, scenes, every counters)
  - `table.c/h` - Command parsing and execution
  - `teletype.c/h` - Main interpreter entry points
  - `command.c/h` - Command handling
  - `match_token.rl` - Ragel state machine for tokenizing (generated: match_token.c)
  - `scanner.rl` - Ragel scanner definition (generated: scanner.c)
  - `ops/` - Operation (OP) and Modifier (MOD) implementations (~70 files)

- **`module/`** - Hardware-specific code for the Eurorack module
  - `main.c` - Module entry point and hardware initialization
  - Mode-specific files: `edit_mode.c`, `live_mode.c`, `help_mode.c`, etc.
  - `grid.c` - Monome grid integration
  - UI and input handling

- **`tests/`** - Unit tests using "greatest" framework
  - Tests core algorithm independent of hardware

- **`simulator/`** - Standalone command parser/simulator
  - Builds independently from module code
  - Useful for testing algorithm changes without firmware build

- **`libavr32/`** - Monome's AVR32 hardware abstraction library (git submodule)

### Key Architectural Patterns

**Parser & Execution Flow:**
1. `match_token.rl` (Ragel) → generates lexer in `match_token.c`
2. Input tokens parsed and matched to OPs/MODs
3. `table.c` executes commands and manages control flow (if/then/else, loops)
4. `state.c` maintains all runtime state

**Operations System:**
- **OPs** (operators) - callable functions with syntax like `COMMAND value` (e.g., `N 0 1` sets CV)
- **MODs** (modifiers/pre-ops) - prefix operators that modify following OPs (e.g., `IF x > 5: THEN OP`)
- Registered in `src/ops/op.c` in tables `tele_ops` and `tele_mods`
- Each OP/MOD is a `tele_op_t` or `tele_mod_t` struct

**Ragel State Machines:**
- `.rl` files define Ragel state machines
- Must be recompiled to `.c` when modified
- Tests and simulator Makefiles handle auto-generation
- Key files: `match_token.rl`, `scanner.rl`

## Adding a New OP or MOD

When adding a new operation or modifier:

1. **Create the implementation** in `src/ops/` (e.g., `src/ops/myop.c` + `src/ops/myop.h`)

2. **Register in six places:**
   - `src/ops/op.c` - Add reference to your struct in `tele_ops` or `tele_mods` table
   - `src/ops/op_enum.h` - Run `python3 utils/op_enums.py` to auto-generate (DO NOT edit manually)
   - `src/match_token.rl` - Add token matching entry (maintain sensible ordering)
   - `module/config.mk` - Add `.c` file to `CSRCS` list
   - `tests/Makefile` - Add `.o` file reference in `tests:` recipe
   - `simulator/Makefile` - Add `.o` file reference in `OBJS` list

3. **Generate enum** with: `python3 utils/op_enums.py`

4. **Verify** with: `cd tests && make test` - includes a check that all registrations are correct

5. **Surface it to users** (not covered by the registration self-check):
   - `module/help_mode.c` - Add the op to the relevant on-module help page and bump that page's `HELPn_LENGTH`
   - `docs/ops/*.toml` - Add an entry (`prototype` + `short`) so the op appears in the generated manual instead of its "Missing documentation" list. Op names with a `#` suffix map from a `_POUND` struct name (see `utils/common/__init__.py`)

## Testing & CI

**CI Pipeline** (.github/workflows/ci.yml):
- Clang-format check
- Unit tests
- Simulator build
- Firmware build
- Documentation build
- Release zip creation

**Test Framework:** "greatest" (embedded in `tests/greatest/`)
- Simple assertion-based unit testing
- Run from `tests/` directory

## Code Formatting

Uses `clang-format` with `.clang-format` configuration at repo root.

- `make format` - Format only changed files (preferred for commits)
- `make format-all` - Format entire codebase

CI verifies formatting on all commits.

## Important Notes

- **Flash budget is tight:** the AVR32 UC3B0512 program flash sits below the reserved NVRAM region (`__flash_nvram_size__` in `module/config.mk`, currently 132K). The NVRAM size is the lever: it must be ≥ `sizeof(nvram_data_t)` (dominated by `scenes[SCENE_SLOTS]` at ~6.5 KB/slot), and lowering it frees program flash. As of the MP-ops work (`SCENE_SLOTS` 18→16, NVRAM 142K→132K) there is ~9.6 KB of free program flash. Changing `SCENE_SLOTS` alters the flash layout, so bump `FIRSTRUN_KEY` (`flash.c`) to force a reseed. Large `const` tables are the real cost (a full Marbles beta port needs ~68 KB of inverse-CDF tables and does not fit — only the single-table fast path does). Measure headroom from `teletype.elf` PT_LOAD LMAs (`.data` LMA end vs `.flash_nvram` start in the map) before adding table-heavy features.
- **Shared object files:** host tests and the AVR32 firmware build share `src/*.o`. Run `make clean` when switching between them or you'll link the wrong architecture's objects.
- **Submodules:** libavr32 and unity (test framework) are git submodules. Clone with `--recursive`.
- **Ragel generation:** If changing `.rl` files, they must be recompiled. The test/simulator Makefiles do this automatically.
- **State serialization:** `scene_serialization.c` handles saving/loading scenes. Serialization format changes require careful versioning.
- **Line endings:** Windows users should clone with `--config core.autocrlf=input` to avoid CRLF issues with build scripts.
