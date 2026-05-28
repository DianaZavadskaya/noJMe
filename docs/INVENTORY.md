# noJMe — Migration Inventory Report

> Generated: 2026-05-27

## 1. Module Summary

### Source Modules (`.c` files)

| Module | Files | Total Lines | Complexity |
|---|---|---|---|
| `src/jvm/` | 11 | 31,110 | **HIGH** (native.c = 12,828; opcodes.c = 5,251) |
| `src/midp/` | 6 | 26,938 | **HIGH** (mobile3d.c = 11,037; display.c = 7,043) |
| `src/libretro/` | 2 | 2,749 | MEDIUM |
| `src/sdl/` | 2 | 2,130 | MEDIUM |
| `src/midi/` | 1 | 863 | LOW |
| `src/utils/` | 3 | 8,134 | **HIGH** (miniz.c = 7,910 — vendored) |
| `src/` (root) | 1 | 1,090 | MEDIUM |
| **Total .c** | **26** | **73,014** | |

### Header Modules (`include/` — public API)

| Header | Lines | Role |
|---|---|---|
| `include/jvm.h` | 583 | Central type hub — `JavaClass`, `JavaObject`, `JavaThread`, `JVM`, etc. |
| `include/classfile.h` | 181 | Class-file parsing API |
| `include/heap.h` | 225 | Allocator/GC API |
| `include/native.h` | 229 | Native method dispatch |
| `include/opcodes.h` | 443 | Bytecode opcode constants, `ACC_*` flags, type codes |
| `include/threads.h` | 169 | Cooperative thread API |
| `include/midp.h` | 376 | MIDP2 API surface (depends on `jvm.h`) |
| `include/sdl_backend.h` | 254 | Backend abstraction (depends on `jvm.h`, `midp.h`) |
| `include/midi.h` | 197 | MIDI synthesizer API |
| `include/debug.h` | 322 | Debug/logging macros and globals |
| `include/libretro.h` | 371 | Libretro protocol types (self-contained) |
| `include/miniz.h` | 1,510 | Vendored ZIP/zlib (self-contained) |
| `include/stb_image.h` | 7,988 | Vendored image decoder (self-contained) |
| `include/mock-sdl/SDL.h` | 271 | Minimal SDL2 stub for no-SDL builds |

### Internal Headers

| Header | Lines | Role |
|---|---|---|
| `src/include/debug_macros.h` | 258 | `LOG_*` macro expansions |
| `src/include/drm_bypass.h` | 135 | DRM bypass internal API |
| `src/include/zlib.h` | 2,057 | Vendored zlib header (part of miniz bundle) |
| `src/include/zutil.h` | 331 | Vendored zlib utilities |
| `src/libretro/libretro_shared.h` | 31 | Shared state between libretro.c and sdl_backend_stubs.c |
| `src/midp/bitmap_font.h` | 737 | Embedded bitmap font data |

---

## 2. Per-File Inventory

### `src/jvm/` — JVM Core

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `native.c` | 12,828 | All Java native methods: `java.lang.*`, `java.io.*`, `javax.microedition.*` | `native.h`, `jvm.h`, `heap.h`, `threads.h`, `opcodes.h`, `classfile.h`, `sdl_backend.h`, `midp.h` |
| `opcodes.c` | 5,251 | Full 256-opcode bytecode interpreter | `opcodes.h`, `jvm.h`, `classfile.h`, `heap.h`, `native.h`, `drm_bypass.h`, `threads.h`, `debug.h` |
| `heap.c` | 2,801 | Mark-sweep GC with free list, thread-safe allocator | `heap.h`, `jvm.h`, `opcodes.h`, `classfile.h`, `native.h`, `midp.h`, pthreads |
| `threads.c` | 1,890 | Cooperative multithreading via `ucontext/makecontext/swapcontext` | `threads.h`, `jvm.h`, `debug.h`, `ucontext.h`, pthreads |
| `nokia_m3d.c` | 1,108 | `com.nokia.mid.m3d.M3D` native software 3D renderer | `native.h`, `jvm.h`, `heap.h`, `midp.h` |
| `execute.c` | 1,278 | Frame execution engine, exception handling, method invocation | `jvm.h`, `classfile.h`, `opcodes.h`, `heap.h`, `threads.h`, `native.h` |
| `jvm.c` | 996 | Class loader, resolution, method dispatch, JAR loading | `jvm.h`, `classfile.h`, `heap.h`, `threads.h`, `opcodes.h`, `native.h`, `miniz.h` |
| `stubs.c` | 4,404 | No-op stub classes for unimplemented J2ME APIs | `jvm.h`, `classfile.h`, `native.h`, `opcodes.h` |
| `classfile.c` | 807 | `.class` file parser (constant pool, method tables) | `classfile.h`, `jvm.h`, `opcodes.h`, `native.h` |
| `method_cache.c` | 325 | FNV-1a hash caches for method/class lookup | `jvm.h`, `classfile.h` |
| `debug_var.c` | 46 | `g_j2me_runtime_debug` flag + `log_lock` mutex definition | pthreads / `<windows.h>` |
| `drm_bypass.c` | 376 | DRM protection bypass (IMEI/IMSI spoof, hideEmulation) | `drm_bypass.h`, `debug.h` |

### `src/midp/` — MIDP2 Platform API

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `mobile3d.c` | 11,037 | JSR-184 M3G full retained-mode 3D pipeline (largest file) | `jvm.h`, `native.h`, `heap.h`, `opcodes.h`, `midp.h`, `sdl_backend.h`, `miniz.h` |
| `display.c` | 7,043 | GameCanvas, Sprite, TiledLayer, LayerManager, display loop | `midp.h`, `jvm.h`, `native.h`, `heap.h`, `sdl_backend.h`, `threads.h`, `bitmap_font.h` |
| `form.c` | 3,992 | Form, List, TextBox, Alert (MIDP2 high-level UI) | `midp.h`, `jvm.h`, `native.h`, `heap.h`, `sdl_backend.h`, `threads.h`, `opcodes.h` |
| `rms.c` | 1,841 | RecordStore persistence (filesystem-backed) | `midp.h`, `jvm.h`, `native.h`, `heap.h`, `opcodes.h`, `<sys/stat.h>` |
| `media.c` | 1,759 | JSR-135 Mobile Media API (MIDI + WAV/PCM), audio thread | `jvm.h`, `native.h`, `midi.h`, `heap.h`, `opcodes.h`, pthreads |
| `graphics.c` | 1,266 | 2D drawing primitives (lines, arcs, images), `stb_image` integration | `midp.h`, `jvm.h`, `bitmap_font.h`, `stb_image.h` |

### `src/libretro/` — RetroArch / Libretro Backend

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `libretro.c` | 1,613 | Libretro core entry points (`retro_init`, `retro_run`, `retro_load_game`, etc.) | `libretro.h`, `jvm.h`, `midp.h`, `native.h`, `opcodes.h`, `sdl_backend.h`, `miniz.h` |
| `sdl_backend_stubs.c` | 1,136 | SDL2 function replacements for libretro build (audio thread, double-buffer) | `sdl_backend.h`, `midp.h`, `libretro.h`, `miniz.h`, `bitmap_font.h` |

### `src/sdl/` — SDL2 Standalone Backend

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `sdl_graphics.c` | 1,731 | SDL2 renderer, input handling, audio output, bitmap font rendering | `sdl_backend.h`, `midp.h`, `midi.h`, `debug.h`, `bitmap_font.h`, pthreads |
| `sdl_headless.c` | 399 | No-display headless backend stub (for CI/testing) | `sdl_backend.h`, `midp.h` |

### `src/midi/` — MIDI Synthesizer

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `midi.c` | 863 | FM synthesis MIDI player | `midi.h`, `<math.h>` |

### `src/utils/` — Utilities & Vendored Libs

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `miniz.c` | 7,910 | Vendored ZIP/zlib decompressor | `miniz.h` (self-contained) |
| `stb_image_impl.c` | 35 | Single TU to compile `stb_image.h` | `stb_image.h` |
| `utils.c` | 189 | String and byte utilities | `jvm.h`, `heap.h`, `debug.h` |

### `src/` (root)

| File | Lines | Responsibility | Key Dependencies |
|---|---|---|---|
| `main.c` | 1,090 | SDL2 standalone entry point, JAR loading, MIDlet auto-detection | `jvm.h`, `classfile.h`, `opcodes.h`, `heap.h`, `native.h`, `midp.h`, `sdl_backend.h`, `miniz.h` |

---

## 3. Dependency Graph

```
stdlib / pthreads / POSIX
        |
        v
[debug_var.c]  ──────────────────────────────────────────────┐
[debug.h / debug_macros.h]  (included by almost everything)  |
        |                                                     |
        v                                                     |
[miniz.h / miniz.c]  (vendored, self-contained)              |
[stb_image.h / stb_image_impl.c]  (vendored, self-contained) |
        |                                                     |
        v                                                     |
[include/jvm.h]  ← CENTRAL TYPE HUB                          |
        |                                                     |
   ┌────┴──────────────────────────────────────────┐          |
   v                                               v          |
[opcodes.h]                                    [heap.c]       |
[classfile.c / classfile.h]                    [threads.c]    |
   |                                               |          |
   v                                               |          |
[method_cache.c]                                  |          |
   |                                               |          |
   └─────────────┬─────────────────────────────────┘          |
                 v                                            |
           [drm_bypass.c]                                    |
           [stubs.c]                                         |
           [native.c]  ←── largest file (12,828 lines)       |
           [nokia_m3d.c]                                     |
           [opcodes.c] ──────────────────────────────────────┘
           [execute.c]
           [jvm.c]  ← uses miniz for JAR loading
                 |
                 v
         [include/midp.h]  (depends on jvm.h)
                 |
    ┌────────────┴──────────────────────┐
    v                                   v
[midp/graphics.c]               [midp/display.c]
[midp/form.c]                   [midp/rms.c]
[midp/media.c] ←── midi.c       [midp/mobile3d.c]
                 |
                 v
         [include/sdl_backend.h]  (depends on jvm.h + midp.h)
                 |
       ┌─────────┴──────────────────┐
       v                            v
[sdl/sdl_graphics.c]      [libretro/sdl_backend_stubs.c]
[sdl/sdl_headless.c]               |
       |                           v
       v                  [libretro/libretro.c]
[src/main.c]              (retro_init / retro_run / retro_load_game)
(int main())
```

---

## 4. Entry Points & Exported Symbols

| Symbol | File | Type |
|---|---|---|
| `main()` | `src/main.c:969` | SDL2 standalone entry point |
| `retro_init()` | `src/libretro/libretro.c:756` | Libretro core init |
| `retro_run()` | `src/libretro/libretro.c:1351` | Libretro per-frame call |
| `retro_load_game()` | `src/libretro/libretro.c:1066` | Libretro JAR loader |
| `retro_load_game_special()` | `src/libretro/libretro.c:1212` | Libretro special loader |
| All 22 `retro_*` symbols | `src/libretro/link.T` | Exported via linker version script — all other symbols hidden |

---

## 5. External / Vendored Dependencies

| Library | Location | Version/Notes |
|---|---|---|
| **miniz** | `include/miniz.h` + `src/utils/miniz.c` | RAD Game Tools / Valve, 2013–2014; ZIP + zlib |
| **stb_image** | `include/stb_image.h` + `src/utils/stb_image_impl.c` | Sean Barrett single-header; PNG/JPEG/BMP |
| **zlib headers** | `src/include/zlib.h` + `src/include/zutil.h` | Bundled alongside miniz |
| **SDL2** | System library (not vendored) | Required only for `make app`; absent in libretro build |
| **pthreads** | System library | Used in heap.c, threads.c, native.c, media.c, sdl_graphics.c, libretro/sdl_backend_stubs.c |

The mock-SDL stub at `include/mock-sdl/SDL.h` (271 lines) is a project-internal fake used by the libretro build in place of real SDL2 headers.

---

## 6. Migration Priority Order (simple-first, core-last)

| Priority | Module | Lines | Rationale |
|---|---|---|---|
| 1 | `src/utils/utils.c` | 189 | No state, pure helpers |
| 2 | `src/jvm/debug_var.c` | 46 | Single global + mutex; no logic |
| 3 | `src/utils/stb_image_impl.c` | 35 | Thin wrapper over vendored header |
| 4 | `src/jvm/drm_bypass.c` | 376 | Self-contained, no cross-module state writes |
| 5 | `src/jvm/method_cache.c` | 325 | Hash-table lookup only |
| 6 | `src/midi/midi.c` | 863 | Self-contained FM synth |
| 7 | `src/sdl/sdl_headless.c` | 399 | Stub-only backend, no display/audio |
| 8 | `src/midp/graphics.c` | 1,266 | 2D primitives; no thread state |
| 9 | `src/midp/rms.c` | 1,841 | Filesystem I/O; isolated from render path |
| 10 | `src/jvm/classfile.c` | 807 | Parser; reads input, writes into `JVM` struct |
| 11 | `src/jvm/stubs.c` | 4,404 | Many small stubs; no logic |
| 12 | `src/midp/media.c` | 1,759 | Audio; touches midi.h and pthreads |
| 13 | `src/jvm/threads.c` | 1,890 | Cooperative scheduler; `ucontext` dependency |
| 14 | `src/jvm/jvm.c` | 996 | Class loader/resolver |
| 15 | `src/jvm/execute.c` | 1,278 | Frame engine; calls opcodes + threads |
| 16 | `src/midp/form.c` | 3,992 | High-level UI; touches SDL backend + threads |
| 17 | `src/jvm/heap.c` | 2,801 | GC; thread-safe, many inward dependencies |
| 18 | `src/sdl/sdl_graphics.c` | 1,731 | Full SDL2 render loop |
| 19 | `src/libretro/sdl_backend_stubs.c` | 1,136 | SDL stubs + libretro audio thread |
| 20 | `src/main.c` | 1,090 | SDL2 standalone entry; glues all layers |
| 21 | `src/libretro/libretro.c` | 1,613 | Libretro entry; glues all layers |
| 22 | `src/midp/display.c` | 7,043 | Canvas/Sprite/Layer; depends on almost everything |
| 23 | `src/jvm/nokia_m3d.c` | 1,108 | Nokia-specific 3D; depends on midp + native |
| 24 | `src/jvm/opcodes.c` | 5,251 | Full bytecode interpreter; deeply coupled |
| 25 | `src/midp/mobile3d.c` | 11,037 | JSR-184 M3G; largest MIDP file |
| 26 | `src/jvm/native.c` | 12,828 | Largest file; all native methods; migrate last |
