#!/usr/bin/env bash
# Runs every tests/cases/NAME.spire through ./spire and compares stdout+stderr
# with tests/cases/NAME.expected. `tests/run.sh --update` rewrites the
# expected files from the current output (review the diff before committing).
set -u
cd "$(dirname "$0")/.."
SPIRE=./spire
[ -x "$SPIRE" ] || { echo "build first: make" >&2; exit 2; }
export HOME="$(mktemp -d)"   # no user config / history leaks into tests
trap 'rm -rf "$HOME"' EXIT
pass=0; fail=0
for t in tests/cases/*.spire; do
    name="${t%.spire}"
    actual="$("$SPIRE" "$t" 2>&1)"
    if [ "${1:-}" = "--update" ]; then
        printf '%s\n' "$actual" > "$name.expected"
        echo "updated $name.expected"
        continue
    fi
    if diff -u "$name.expected" <(printf '%s\n' "$actual") > /tmp/spire-test-diff.$$; then
        printf '  \033[32mpass\033[0m %s\n' "${name##*/}"
        pass=$((pass + 1))
    else
        printf '  \033[31mFAIL\033[0m %s\n' "${name##*/}"
        sed 's/^/      /' /tmp/spire-test-diff.$$
        fail=$((fail + 1))
    fi
done
rm -f /tmp/spire-test-diff.$$
[ "${1:-}" = "--update" ] && exit 0
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
