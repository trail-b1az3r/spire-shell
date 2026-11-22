#!/bin/sh
mode="${1:-}"
# Runs every tests/NAME.sh and NAME.fish through ./spire and compares the
# output with NAME.expected.  tests/run.sh --update rewrites the expected files.
cd "$(dirname "$0")/.." || exit 2
home="$(mktemp -d)"
trap 'rm -rf "$home"' EXIT
pass=0; fail=0
for t in tests/*.sh tests/*.fish; do
    case "$t" in tests/run.sh|tests/run-compat.sh) continue ;; esac
    exp="${t%.*}.expected"
    out="$(cd /tmp && HOME="$home" "$OLDPWD/spire" "$OLDPWD/$t" 2>&1)"
    if [ "$mode" = "--update" ]; then printf '%s\n' "$out" > "$exp"; continue; fi
    if [ "$out" = "$(cat "$exp")" ]; then
        printf '  \033[32mpass\033[0m %s\n' "$t"; pass=$((pass + 1))
    else
        printf '  \033[31mFAIL\033[0m %s\n' "$t"; fail=$((fail + 1))
        printf '%s\n' "$out" | diff "$exp" - | sed 's/^/      /'
    fi
done
[ "$mode" = "--update" ] && exit 0
if [ "$fail" -eq 0 ]; then sh tests/run-compat.sh || fail=$((fail + 1)); fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
