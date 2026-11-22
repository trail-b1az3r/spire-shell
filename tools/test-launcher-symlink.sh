#!/usr/bin/env sh
# Verify that invoking Spire through a symlink resolves the real project path.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/fake-whetstone" <<'SH'
#!/usr/bin/env sh
printf '%s\n' "$@" > "$CAPTURE"
SH
chmod +x "$TMP/fake-whetstone"
mkdir -p "$TMP/linkdir"
ln -s "$ROOT/spire" "$TMP/linkdir/spire-link"
CAPTURE="$TMP/args" WHETSTONE="$TMP/fake-whetstone" sh "$TMP/linkdir/spire-link" -c "echo test"
[ -s "$TMP/args" ] || { echo "FAIL: runtime was not invoked" >&2; exit 1; }
first="$(sed -n '1p' "$TMP/args")"
project="$(sed -n '2p' "$TMP/args")"
[ "$first" = "run" ] || { echo "FAIL: expected run subcommand, got $first" >&2; exit 1; }
[ "$project" = "$ROOT" ] || { echo "FAIL: symlink project path incorrect: $project" >&2; exit 1; }
echo "PASS: symlink launcher resolves project root"
