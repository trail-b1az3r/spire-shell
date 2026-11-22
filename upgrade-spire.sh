#!/usr/bin/env sh
# Upgrade the system installation to the latest z#nd branch and rebuild.
set -eu
if [ "$(id -u)" -ne 0 ]; then
 echo "spire-upgrade: run with sudo (sudo spire-upgrade)" >&2
 exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
SPIRE_REPO="https://github.com/trail-b1az3r/spire-shell.git"
WHT_REPO="https://github.com/trail-b1az3r/z-nd.git"
BRANCH="z#nd"
echo "Fetching Spire source..."
git clone --depth 1 --branch "$BRANCH" "$SPIRE_REPO" "$TMP/spire"
echo "Fetching Whetstone..."
git clone --depth 1 --branch claude/spire-in-zsharpend "$WHT_REPO" "$TMP/z-nd"
(cd "$TMP/z-nd" && cargo build --release -p whetstone)
export WHETSTONE="$TMP/z-nd/target/release/whetstone"
echo "Building Spire bundle..."
sh "$TMP/spire/compile-spire.sh"
version="$(cat "$TMP/spire/VERSION" 2>/dev/null || echo unknown)"
install -d /usr/local/lib/spire-shell /usr/local/bin
install -m 755 "$WHETSTONE" /usr/local/bin/whetstone
cp -R "$TMP/spire"/. /usr/local/lib/spire-shell/
cat > /usr/local/bin/spire <<'LAUNCH'
#!/usr/bin/env sh
exec whetstone run /usr/local/lib/spire-shell/build/spire.wsb -- "$@"
LAUNCH
chmod 755 /usr/local/bin/spire
echo "Updated Spire to $version."
