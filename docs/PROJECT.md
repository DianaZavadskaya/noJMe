# Project Description: noJMe

noJMe is a J2ME (MIDP2/CLDC 1.1) emulator written in pure C11. It runs J2ME mobile Java games and apps either as a standalone SDL2 application or as a Libretro core for RetroArch.

## Features

- Full JVM: all 256 bytecode opcodes, Java 1.3 / CLDC 1.1 / MIDP 2.0
- MIDP2 API: Graphics, Display, GameCanvas, Sprite, TiledLayer, LayerManager
- M3G (JSR-184) 3D graphics pipeline
- RMS persistence (RecordStore)
- MIDI/WAV/PCM audio (FM synthesis)
- DRM bypass (Digital Chocolate, Siemens, Gameloft, EA)
- Libretro Core for RetroArch
- Headless mode for CI/testing

## Architecture

### Layer Structure (bottom to top)

```
JAR/ZIP loading (miniz)
  └─ JVM Core (src/jvm/)
       ├─ classfile.c   — .class file parser (constant pool, bytecode extraction)
       ├─ jvm.c         — class loader, resolution, method dispatch
       ├─ heap.c        — bump-pointer allocator + mark-and-sweep GC
       ├─ execute.c     — frame execution, exception handling
       ├─ opcodes.c     — full 256-opcode bytecode interpreter
       ├─ threads.c     — cooperative threading (yield every 1000 opcodes)
       ├─ native.c      — all native Java methods (java.lang.*, javax.microedition.*)
       ├─ stubs.c       — no-op stubs for unimplemented APIs
       └─ drm_bypass.c  — IMEI/IMSI spoofing, hideEmulation mode
  └─ MIDP2 API (src/midp/)
       ├─ graphics.c    — 2D drawing primitives
       ├─ display.c     — GameCanvas, Sprite, TiledLayer, LayerManager
       ├─ form.c        — Form, List, TextBox, Alert
       ├─ media.c       — MIDI + WAV/PCM audio
       ├─ rms.c         — RecordStore persistence
       └─ mobile3d.c    — JSR-184 M3G full 3D pipeline (largest file: 518 KB)
  └─ Platform Backend
       ├─ src/sdl/      — SDL2 renderer, input, audio (standalone)
       └─ src/libretro/ — RetroArch core interface
```

### Key Design Facts

- **`include/jvm.h`** is the single type hub — `JavaClass`, `JavaObject`, `JavaArray`, `JavaThread`, `JavaFrame`, `JavaMethod`, `JavaField`, `JVM` are all defined here and imported by every module.
- **Threading is cooperative, not preemptive.** `threads.c` yields every `YIELD_INTERVAL` (1000) opcodes. No actual POSIX threads are created per Java thread.
- **GC object headers** use `0xDEADBEEF` magic for corruption detection.
- **Debug logging** is guarded by compile-time `J2ME_DEBUG` and a runtime flag `g_j2me_runtime_debug` (in `jvm/debug_var.c`). All log output uses `log_lock`/`log_unlock` for thread safety.
- **Libretro builds** use `src/libretro/link.T` (linker version script) to export only `retro_*` symbols. SDL functions are replaced by stubs in `libretro/sdl_backend_stubs.c`.
- **Two frontends share the same core:** `src/main.c` (SDL2 standalone) and `src/libretro/libretro.c` (RetroArch). Both load a JAR, auto-detect the MIDlet from `MANIFEST.MF`, and drive the JVM.

### Adding Native Methods

Native Java methods are implemented in `src/jvm/native.c` (498 KB). Unimplemented methods get no-op stubs in `src/jvm/stubs.c`. When adding a new API class, add its full implementation to `native.c` or a new file; add minimal stubs to `stubs.c` only if the method truly does nothing.

### Platform Defines

| Define | When set |
|---|---|
| `WIN32` | Windows MinGW build |
| `J2ME_DEBUG` | Debug logging enabled (0=off, 1=on) |
| `__USE_MINGW_ANSI_STDIO=1` | MinGW printf compatibility |
| `_GNU_SOURCE` | Linux build |
