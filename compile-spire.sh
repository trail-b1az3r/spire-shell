#!/usr/bin/env sh
# Build the Spire Z# program into a Whetstone bundle (.wsb).
# Usage: ./compile-spire.sh [--run] [--build-whetstone] [project-dir]
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
RUN_AFTER=0
BUILD_WHETSTONE=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --run) RUN_AFTER=1; shift ;;
    --build-whetstone|--release-whetstone) BUILD_WHETSTONE=1; shift ;;
    -h|--help)
      echo "Usage: $0 [--run] [--build-whetstone] [project-dir]"
      echo "  --run               run the built bundle after compiling"
      echo "  --build-whetstone    build Whetstone from a neighboring z-nd checkout"
      exit 0 ;;
    *) PROJECT_DIR="$(CDPATH= cd -- "$1" && pwd)"; shift ;;
  esac
done

WHT="${WHETSTONE:-}"
if [ -z "$WHT" ]; then
  WHT="$(command -v whetstone 2>/dev/null || true)"
fi

if [ -z "$WHT" ] && [ "$BUILD_WHETSTONE" -eq 1 ]; then
  ZND_DIR="$SCRIPT_DIR/../z-nd"
  if [ ! -d "$ZND_DIR" ]; then
    echo "compile-spire: neighboring z-nd checkout not found at $ZND_DIR" >&2
    echo "Set WHETSTONE=/path/to/whetstone or clone z-nd beside spire-shell." >&2
    exit 127
  fi
  (cd "$ZND_DIR" && cargo build --release -p whetstone)
  WHT="$ZND_DIR/target/release/whetstone"
fi

if [ -z "$WHT" ] || [ ! -x "$WHT" ]; then
  echo "compile-spire: Whetstone executable not found." >&2
  echo "Install/build Whetstone or set WHETSTONE=/path/to/whetstone." >&2
  echo "Optional: $0 --build-whetstone" >&2
  exit 127
fi

"$WHT" check "$PROJECT_DIR"
"$WHT" build "$PROJECT_DIR"

BUNDLE="$PROJECT_DIR/build/spire.wsb"
if [ ! -f "$BUNDLE" ]; then
  echo "compile-spire: expected output bundle missing: $BUNDLE" >&2
  exit 1
fi

echo "Built Spire bundle: $BUNDLE"
if [ "$RUN_AFTER" -eq 1 ]; then
  exec "$WHT" run "$BUNDLE"
fi
