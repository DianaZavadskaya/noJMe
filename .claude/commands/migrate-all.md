---
name: migrate-all
description: Orchestrates migration of all pending C modules to JavaScript in priority order. Reads MIGRATION_PLAN.md to find the next uncompleted module, runs the full cycle via migrate-module, updates the plan, then moves to the next. Usage: /migrate-all (next module) or /migrate-all phase=1 (full phase).
---

Orchestrate JavaScript migration for all pending modules.

## Arguments

- No arguments → migrate the next single uncompleted module from the plan
- `phase=N` → migrate all modules in phase N (e.g. `phase=1`)
- `all` → migrate all remaining uncompleted modules in order (use with caution — long-running)

## Phase map (from docs/INVENTORY.md priority order)

| Phase | Modules | Model |
|---|---|---|
| 1 | utils, debug_var, stb_image_impl, drm_bypass, method_cache, midi, sdl_headless | Haiku 4.5 |
| 2 | classfile, rms | Sonnet 4.6 |
| 3 | media, stubs | Sonnet 4.6 |
| 4 | threads, heap | Sonnet 4.6 |
| 5 | jvm, execute | Sonnet 4.6 |
| 6 | graphics, form | Sonnet 4.6 |
| 7 | sdl_graphics, sdl_backend_stubs, main, libretro | Sonnet 4.6 |
| 8 | opcodes | Opus 4.7 |
| 9 | display, nokia_m3d | Opus 4.7 |
| 10 | native | Opus 4.7 |
| 11 | mobile3d | Opus 4.7 |

## Steps

1. Read `docs/MIGRATION_PLAN.md` to find pending modules (unchecked items under step 4)
2. If step 4 checklist does not exist yet, initialize it in MIGRATION_PLAN.md using the phase map above — all items unchecked
3. Determine the target list based on arguments:
   - no args → first unchecked item only
   - `phase=N` → all unchecked items in phase N
   - `all` → all unchecked items in order
4. For each target module in order:
   a. Print: `→ Migrating <module> (<phase>, <model>)...`
   b. Run `/capture-baseline <module>`
   c. Run `/migrate-module <module>`
   d. Run `/check-regression <module>`
   e. If PASS: mark `[x]` in MIGRATION_PLAN.md, print `✓ <module> done`
   f. If FAIL: print `✗ <module> FAILED — stopping`, do not continue to next module, report what broke
5. After all targets: print a summary table — module | status | js output path

## Safety rules

- Always stop on first FAIL — do not migrate the next module with a broken baseline
- Never overwrite an already-migrated `js/` file without re-running `/capture-baseline` first
- If `tests/baselines/<module>.sha256` does not exist, run `/capture-baseline` before migrating
