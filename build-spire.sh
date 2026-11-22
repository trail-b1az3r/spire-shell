#!/usr/bin/env sh
# Spire version/build helper.
# Default: increment patch. Use --minor or --major to manually select that bump.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$ROOT"
VERSION_FILE="$ROOT/VERSION"
[ -f "$VERSION_FILE" ] || { echo "build-spire: VERSION missing" >&2; exit 1; }
current="$(cat "$VERSION_FILE" | tr -d '\r\n')"
case "$current" in
  *.*.* ) ;;
  * ) echo "build-spire: invalid semver in VERSION: $current" >&2; exit 1 ;;
esac
major="$(printf '%s' "$current" | cut -d. -f1)"
minor="$(printf '%s' "$current" | cut -d. -f2)"
patch="$(printf '%s' "$current" | cut -d. -f3)"
case "$major:$minor:$patch" in *[!0-9:]* ) echo "build-spire: VERSION must be numeric semver" >&2; exit 1;; esac
bump="patch"
install=0
run=0
while [ "$#" -gt 0 ]; do
 case "$1" in
  --major) bump=major; shift ;;
  --minor) bump=minor; shift ;;
  --patch) bump=patch; shift ;;
  --install) install=1; shift ;;
  --run) run=1; shift ;;
  -h|--help)
    echo "Usage: $0 [--major|--minor|--patch] [--install] [--run]"
    echo "Default auto-increments patch; major/minor are manual and reset lower fields."
    exit 0 ;;
  *) echo "build-spire: unknown option: $1" >&2; exit 2 ;;
 esac
done
case "$bump" in
 major) major=$((major + 1)); minor=0; patch=0 ;;
 minor) minor=$((minor + 1)); patch=0 ;;
 patch) patch=$((patch + 1)) ;;
esac
next="$major.$minor.$patch"
printf '%s\n' "$next" > "$VERSION_FILE"
echo "Spire version: $current -> $next ($bump bump)"
sh "$ROOT/compile-spire.sh" --build-whetstone
echo "$next" > "$ROOT/build/VERSION"
if [ "$install" -eq 1 ]; then
 if [ "$(id -u)" -ne 0 ]; then echo "Use sudo $0 --install" >&2; exit 1; fi
 install -d /usr/local/lib/spire-shell /usr/local/bin
 install -m 755 "$WHETSTONE" /usr/local/bin/whetstone
 cp -R "$ROOT"/. /usr/local/lib/spire-shell/
 cat > /usr/local/bin/spire <<'LAUNCH'
#!/usr/bin/env sh
exec whetstone run /usr/local/lib/spire-shell/build/spire.wsb -- "$@"
LAUNCH
 chmod 755 /usr/local/bin/spire
 install -m 755 "$ROOT/upgrade-spire.sh" /usr/local/bin/spire-upgrade
 echo "Installed Spire $next to /usr/local/lib/spire-shell"
fi
if [ "$run" -eq 1 ]; then exec "${WHETSTONE:-whetstone}" run "$ROOT/build/spire.wsb"; fi
