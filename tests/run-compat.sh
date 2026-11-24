#!/usr/bin/env sh
# Compatibility smoke tests. Requires a built/working Whetstone runtime.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SPIRE="$ROOT/spire"

[ -f "$SPIRE" ] || { echo "FAIL: spire launcher missing" >&2; exit 1; }

expect_ok() {
    label="$1"; command="$2"
    if sh "$SPIRE" -c "$command" >/dev/null 2>&1; then
        echo "PASS: $label"
    else
        echo "FAIL: $label" >&2
        exit 1
    fi
}
expect_output() {
    label="$1"; command="$2"; expected="$3"
    actual="$(sh "$SPIRE" -c "$command" 2>&1)" || { echo "FAIL: $label (nonzero exit)" >&2; exit 1; }
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $label"
    else
        echo "FAIL: $label (expected '$expected', got '$actual')" >&2
        exit 1
    fi
}
expect_ok "enable_transience compatibility builtin" "enable_transience"
expect_output "echo output" "echo spire-compat-ok" "spire-compat-ok"
expect_output "psub source pipeline" "source (echo \"echo psub-compat-ok\" | psub)" "psub-compat-ok"
echo "Compatibility smoke tests passed."
