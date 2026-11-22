#!/usr/bin/env sh
# Regression checks for shell compatibility builtins and Z# project build.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SPIRE="$ROOT/spire"

if [ ! -x "$SPIRE" ]; then
  echo "test-compat: spire launcher is not executable: $SPIRE" >&2
  exit 1
fi

run_ok() {
  label="$1"; shift
  if "$SPIRE" -c "$*" >/dev/null 2>&1; then
    echo "PASS: $label"
  else
    echo "FAIL: $label" >&2
    exit 1
  fi
}

run_ok "enable_transience is accepted" "enable_transience"

echo "Compatibility builtin checks passed."
