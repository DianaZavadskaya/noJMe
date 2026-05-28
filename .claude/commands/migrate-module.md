---
name: migrate-module
description: Orchestrates migration of a single C module to JavaScript. Runs the full cycle: capture-baseline → migrate → check-regression → update MIGRATION_PLAN.md. Usage: /migrate-module <module-name>  (e.g. /migrate-module utils or /migrate-module rms)
---

Migrate the C module specified in $ARGUMENTS to JavaScript.

## Model selection

Choose the agent model based on the module's complexity (from docs/INVENTORY.md):

| Phase | Modules | Model |
|---|---|---|
| 1–3 (leaf) | utils, debug_var, stb_image_impl, drm_bypass, method_cache, midi, sdl_headless | claude-haiku-4-5-20251001 |
| 4–9 (core) | classfile, rms, media, stubs, threads, heap, jvm, execute, form, sdl_graphics | claude-sonnet-4-6 |
| 10–11 (critical) | opcodes, display, nokia_m3d, mobile3d, native | claude-opus-4-7 |

## Steps

1. Read `docs/INVENTORY.md` to find the source file path and dependencies for $ARGUMENTS
2. Run `/capture-baseline $ARGUMENTS` to snapshot current behaviour
3. Spawn the `migrate-module` agent with the source file path
4. Run `/check-regression $ARGUMENTS` to verify no regressions
5. If PASS: update `docs/MIGRATION_PLAN.md` — add the module as completed under step 4
6. If FAIL: report what broke and do not update the plan

## Progress tracking

Under step 4 in `docs/MIGRATION_PLAN.md`, maintain a checklist:

```
- [ ] Phase 1 — utils.c → js/utils/utils.mjs
- [ ] Phase 1 — debug_var.c → js/jvm/debug_var.mjs
...
```

Mark each item `[x]` after a successful `/check-regression`.
