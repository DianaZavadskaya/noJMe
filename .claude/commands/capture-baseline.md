---
name: capture-baseline
description: Capture a regression baseline for a specific module or JAR before migration. Run this before touching any source file in the module. Stores SHA-256 framebuffer hashes in tests/baselines/.
---

Capture a regression baseline for the module or JAR specified by the user.

## Steps

1. Identify the target: if the user named a module (e.g. `rms`, `threads`), find the relevant JAR(s) in `tests/` that exercise it using Glob and Grep.
   If the user named a JAR directly, use it.

2. Check that `bin/j2me-headless` exists. If not, tell the user to run `make headless` first.

3. For each target JAR, run:
   ```
   ./bin/j2me-headless tests/<name>.jar 2>/dev/null | sha256sum > tests/baselines/<name>.sha256
   ```

4. Confirm each baseline file was written and print the hash.

5. Remind the user: do not modify source files until after the baseline is captured.

## Output

Short report:
- Which JARs were baselined
- Hash values stored
- Path to each `.sha256` file
