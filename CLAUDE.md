# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> Project description and architecture: [docs/PROJECT.md](docs/PROJECT.md)

## Build Commands

All builds use `make`. No package manager — dependencies (miniz, stb_image) are vendored.

```bash
make                    # Windows DLL (default): bin/nojme_libretro.dll
make platform=linux     # Linux SO: bin/nojme_libretro.so
make app CC=gcc         # SDL2 standalone (Linux/WSL): bin/j2me-emulator
make headless           # No-display debug build: bin/j2me-headless
make clean              # Remove obj/ and bin/
make check              # Verify all source files are present
make info               # Print build configuration summary
```

**Requirements:** GCC (C11) or MinGW, Make, SDL2 (for `make app` only), pthreads.

**M17 cross-compile (ARM Cortex-A7):**
```bash
make platform=m17 CROSS_COMPILE=arm-linux-gnueabihf-
```

## Running Tests

No automated test framework. Tests are JAR files run manually against the headless build:

```bash
make headless
./bin/j2me-headless tests/TestSuite.jar
```

Run a single JAR file:
```bash
./bin/j2me-headless tests/<name>.jar 2>&1 | tail -20
```

Toggle runtime debug output with **F12** (standalone app) or set `J2ME_DEBUG=1` at compile time.
