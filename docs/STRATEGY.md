# noJMe Migration Strategy Recommendation

> Generated: 2026-05-27

---

## 1. Codebase Scoring

| Dimension | Score (1–5) | Rationale |
|---|---|---|
| **Size** | 5 | 73,014 lines across 26 `.c` files; the two largest files alone (native.c 12,828 + mobile3d.c 11,037) exceed 23,000 lines |
| **Coupling** | 5 | `include/jvm.h` is a single god-header imported by every module; `native.c` depends on 7 headers; midp/ depends on both jvm.h and sdl_backend.h; the dependency graph has no horizontal cuts |
| **Complexity** | 5 | Full 256-opcode JVM interpreter, mark-sweep GC, cooperative ucontext threading, JSR-184 retained-mode 3D pipeline, DRM bypass; cyclomatic complexity is very high in opcodes.c, native.c, mobile3d.c |
| **Entry points** | 2 | Only two distinct frontends: `main()` in src/main.c and the 22 `retro_*` symbols in src/libretro/libretro.c |
| **Test coverage** | 1 | No automated test framework; tests are manual JAR executions; no regression harness |

**Total score: 18 / 25**

---

## 2. Strategy Recommendation

**Strangler Fig** — incremental replacement module by module.

A score of 18/25 rules out Big Bang: `native.c` alone is ~500 KB and dispatches hundreds of interleaved Java API families. A simultaneous cut-over would require holding the entire ported system non-functional for many months.

Strangler Fig is viable because:
1. The two entry points (`main()` and `retro_*`) are narrow and well-defined — a routing shim can forward calls to old or new implementation per module.
2. The inventory's priority ordering (simple-first, core-last) provides a natural strangling sequence.
3. The headless build provides an executable oracle for every migrated module.

---

## 3. Migration Seams

| Seam | Location | Description |
|---|---|---|
| **Vendored libraries** | `include/miniz.h`, `include/stb_image.h` | Self-contained; zero project dependencies. Swap independently at any point. |
| **Debug subsystem** | `src/jvm/debug_var.c` + `include/debug.h` | Single global flag and mutex. Drop-in replacement with same symbol names. |
| **MIDI synthesizer** | `src/midi/midi.c` + `include/midi.h` | No JVM or MIDP types; pure audio math. Replace while `media.c` retains the same `midi.h` contract. |
| **RMS persistence** | `src/midp/rms.c` | Communicates only through `midp.h` native-method dispatch. Filesystem I/O is isolated. |
| **Backend abstraction** | `include/sdl_backend.h` | SDL2 and libretro stubs implement the same contract. New backend can be introduced without touching JVM or MIDP. |
| **Frontend / entry-point** | `src/main.c`, `src/libretro/libretro.c` | Either frontend can be strangled independently. Libretro linker script (`link.T`) enforces 22 exported symbols. |
| **JVM / MIDP interface** | `include/midp.h` | Once `jvm.h` types are stabilised, MIDP modules can migrate one file at a time. |
| **Native method dispatch** | `include/native.h` | Dispatch by name/signature string — individual Java API families can be migrated in isolation. |

---

## 4. Effort Estimate

> Assumptions: 2 engineers; each phase tested against the headless JAR oracle.

| Phase | Modules | Lines | Person-weeks |
|---|---|---|---|
| Phase 0 — Harness & Oracle | headless CI, baseline capture, diff tooling | — | 2 |
| Phase 1 — Leaf utilities | utils.c, debug_var.c, stb_image_impl.c, drm_bypass.c, method_cache.c, midi.c, sdl_headless.c | ~2,233 | 3 |
| Phase 2 — Storage & parsing | classfile.c, rms.c | ~2,648 | 4 |
| Phase 3 — Audio & stubs | media.c, stubs.c | ~6,163 | 6 |
| Phase 4 — Threading & heap | threads.c, heap.c | ~4,691 | 8 |
| Phase 5 — JVM core | jvm.c, execute.c | ~2,274 | 6 |
| Phase 6 — 2D graphics & UI | graphics.c, form.c | ~5,258 | 8 |
| Phase 7 — Backends | sdl_graphics.c, sdl_backend_stubs.c, main.c, libretro.c | ~5,570 | 8 |
| Phase 8 — Opcodes interpreter | opcodes.c | ~5,251 | 10 |
| Phase 9 — Display pipeline | display.c, nokia_m3d.c | ~8,151 | 12 |
| Phase 10 — Native methods | native.c | ~12,828 | 16 |
| Phase 11 — 3D pipeline | mobile3d.c | ~11,037 | 14 |
| **Subtotal** | | ~66,358 | **~97** |
| **Buffer (20%)** | | | **~20** |
| **Total** | | | **~117 person-weeks (~29 person-months)** |

With 2 engineers: **~14–15 calendar months**. With 3 engineers: **~10 calendar months**.

---

## 5. Cost Optimisation

1. **Do not migrate vendored libraries.** `miniz.c` (7,910 lines) + `stb_image.h` (7,988 lines) = ~15,900 lines (22% of total). Re-vendor upstream releases at near-zero cost.

2. **Reuse the headless oracle instead of writing tests from scratch.** Automate frame-capture diffs (pixel hash per JAR per N frames) in Phase 0 — costs 1–2 weeks but eliminates hand-written unit tests across all later phases.

3. **Generate stubs.c automatically.** 4,404 lines of repetitive no-op stubs follow a consistent pattern. A 100-line code generator saves 3–4 person-weeks.

4. **Migrate native.c by Java API family.** Split the 12,828-line file into ~20 focused files by class name (e.g., `java.lang.String`, `java.io.InputStream`). Migrate one family per sprint, validate with targeted JAR tests — avoids a single 16-week blocked sprint.

5. **Defer JSR-184 (mobile3d.c) until demand is confirmed.** At 11,037 lines, the M3G 3D pipeline serves a narrow set of 3D-capable titles. If primary use case is 2D games, Phase 11 can be deferred or replaced with a logging stub — saving up to 14 person-weeks on initial delivery.

---

## 6. Top 3 Risks and Mitigations

### Risk 1 — Cooperative threading semantics (Critical)

`threads.c` uses POSIX `ucontext`/`makecontext`/`swapcontext`. The yield interval (every 1,000 opcodes) is observable by J2ME games relying on timing. A port using OS threads or async/await will alter yield semantics, causing deadlocks or broken game loops.

**Mitigation:** Replicate the cooperative scheduler exactly in Phase 4 using the same ucontext primitives (available on Linux/glibc; emulatable on Windows via fibers). Write a dedicated threading-behaviour JAR test covering `Thread.sleep()`, `Thread.yield()`, and concurrent `RecordStore` access before touching `execute.c` or `opcodes.c`.

---

### Risk 2 — native.c has no internal seam (High)

At 12,828 lines and 7 header dependencies, `native.c` is called continuously during all earlier phases. Regressions discovered in Phase 10 require re-testing every prior phase.

**Mitigation:** During Phase 0, capture per-method invocation logs for a representative JAR suite (via `J2ME_DEBUG=1`). Use this log as a ground-truth call trace. Validate each migrated API family against the call trace in isolation before moving to the next.

---

### Risk 3 — No regression baseline (High)

Current test infrastructure is entirely manual. Regressions introduced during migration will only surface if someone manually tests the specific JAR that exercises the broken code path.

**Mitigation:** Phase 0 must deliver an automated baseline before any migration begins. Minimum viable baseline: for each JAR in `tests/`, run `j2me-headless` for N frames, capture SHA-256 of framebuffer output, store as golden value, run in CI on every PR. Costs 2 person-weeks up front; without this gate the strangler fig degrades to undetected-regression accumulation.
