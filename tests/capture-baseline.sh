#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADLESS="$REPO_ROOT/bin/j2me-headless"
BASELINES="$SCRIPT_DIR/baselines"

cd "$REPO_ROOT"
make headless CC=gcc

mkdir -p "$BASELINES"

for jar in "$SCRIPT_DIR"/*.jar; do
    name="$(basename "$jar" .jar)"
    hash="$("$HEADLESS" "$jar" 300 2>/dev/null | sha256sum | cut -d' ' -f1)"
    printf '%s' "$hash" > "$BASELINES/$name.sha256"
    echo "DONE $name"
done
