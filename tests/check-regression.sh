#!/bin/sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEADLESS="$REPO_ROOT/bin/j2me-headless"
BASELINES="$SCRIPT_DIR/baselines"

failures=0

for jar in "$SCRIPT_DIR"/*.jar; do
    name="$(basename "$jar" .jar)"
    baseline_file="$BASELINES/$name.sha256"

    if [ ! -f "$baseline_file" ]; then
        echo "SKIP $name (no baseline)"
        continue
    fi

    expected="$(cat "$baseline_file")"
    actual="$("$HEADLESS" "$jar" 300 2>/dev/null | sha256sum | cut -d' ' -f1)"

    if [ "$actual" = "$expected" ]; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        echo "     expected: $expected"
        echo "     actual:   $actual"
        failures=$((failures + 1))
    fi
done

[ "$failures" -eq 0 ]
