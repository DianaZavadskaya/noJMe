---
name: check-regression
description: Check for regressions after migrating a module. Compares current framebuffer output against the stored baseline in tests/baselines/. Run after every migration PR.
---

Check for regressions in the module or JAR specified by the user.

## Steps

1. Identify the target: if the user named a module (e.g. `heap`, `display`), find the relevant JAR(s) in `tests/` using Glob and Grep.
   If the user named a JAR directly, use it.

2. Check that `bin/j2me-headless` exists. If not, tell the user to run `make headless` first.

3. Check that a baseline exists in `tests/baselines/<name>.sha256`. If not, tell the user to run `/capture-baseline` first.

4. For each target JAR, run:
   ```
   ./bin/j2me-headless tests/<name>.jar 2>/dev/null | sha256sum
   ```
   Compare against the stored baseline.

5. Report result per JAR:
   - PASS — hashes match
   - FAIL — hashes differ, print both values

6. If any FAIL: suggest next steps — check recent changes with `git diff`, run with `J2ME_DEBUG=1` for verbose output.

## Output

Short report:
- PASS / FAIL per JAR
- On failure: old hash vs new hash, suggested debug steps
- Overall: CLEAN or REGRESSIONS DETECTED
