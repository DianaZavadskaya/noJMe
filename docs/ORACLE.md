# Headless Test Oracle

## What it is

The oracle runs each JAR in `tests/` through `bin/j2me-headless` for 300 frames, captures the SHA-256 of stdout (framebuffer output), and compares against stored golden values in `tests/baselines/`. It is the primary regression gate for the Strangler Fig migration: every module change must leave all hashes unchanged before merging.

## How to run

**Capture a fresh baseline** (do this once before any migration work, and after every intentional behaviour change):

```sh
make baseline
```

This builds the headless binary then runs `tests/capture-baseline.sh`, writing `tests/baselines/<name>.sha256` for every JAR.

**Check for regressions** (run after every change):

```sh
make test
```

This runs `tests/check-regression.sh`. Output is one line per JAR:

```
PASS StringTest
PASS TestSuite
FAIL JavaTest3
     expected: a3f1...
     actual:   9c4b...
```

The target exits non-zero if any JAR fails, suitable for use in CI.

## Adding a new JAR

1. Drop the `.jar` file into `tests/`.
2. Run `make baseline` to generate its golden hash.
3. Commit both the JAR and the new `tests/baselines/<name>.sha256` file.

## Updating a baseline after an intentional behaviour change

When a migration intentionally alters observable output (e.g., a bug fix that changes rendering):

1. Verify the new behaviour is correct by inspecting the JAR manually.
2. Delete the stale baseline: `rm tests/baselines/<name>.sha256`
3. Run `make baseline` to regenerate it (or run `tests/capture-baseline.sh` directly).
4. Commit the updated `.sha256` alongside the source change so reviewers can see the hash delta.

Never regenerate baselines speculatively — only do so after confirming the output change is intentional.
