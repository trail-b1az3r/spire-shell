#!/usr/bin/env sh
# Build/install Spire without changing its version. Versioning is managed by GitHub Actions.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$ROOT"
VERSION_FILE="$ROOT/VERSION"
VERSION="$(cat "$VERSION_FILE" | tr -d '\r\n')"
install_mode=0
run_after=0
while [ "$#" -gt 0 ]; do
 case "$1" in
  --install) install_mode=1; shift ;;
  --run) run_after=1; shift ;;
  -h|--help)
    echo "Usage: $0 [--install] [--run]"
    echo "Builds Spire using the version already recorded in VERSION."
    echo "Version bumps are handled by GitHub Actions."
    exit 0 ;;
  *) echo "build-spire: unknown option: $1" >&2; exit 2 ;;
 esac
done
WHT="${WHETSTONE:-}"
if [ -z "$WHT" ]; then WHT="$(command -v whetstone 2>/dev/null || true)"; fi
if [ -z "$WHT" ] || [ ! -x "$WHT" ]; then
 if [ -d "$ROOT/../z-nd" ]; then unset WHETSTONE; sh "$ROOT/compile-spire.sh" --build-whetstone
 WHT="$ROOT/../z-nd/target/release/whetstone"
 else
 echo "build-spire: Whetstone missing. Set WHETSTONE or clone z-nd beside this repo." >&2
 exit 127
 fi
else
 WHETSTONE="$WHT"
 export WHETSTONE
 sh "$ROOT/compile-spire.sh"
fi
mkdir -p "$ROOT/build"
printf '%s\n' "$VERSION" > "$ROOT/build/VERSION"
if [ "$install_mode" -eq 1 ]; then
 if [ "$(id -u)" -ne 0 ]; then echo "Run with sudo for --install." >&2; exit 1; fi
 install -d /usr/local/lib/spire-shell /usr/local/bin
 install -m 755 "$WHT" /usr/local/bin/whetstone
 cp -R "$ROOT"/. /usr/local/lib/spire-shell/
 cat > /usr/local/bin/spire <<'LAUNCH'
#!/usr/bin/env sh
exec whetstone run /usr/local/lib/spire-shell/build/spire.wsb -- "$@"
LAUNCH
 chmod 755 /usr/local/bin/spire
 install -m 755 "$ROOT/upgrade-spire.sh" /usr/local/bin/spire-upgrade
 echo "Installed Spire $VERSION to /usr/local/lib/spire-shell"
fi
if [ "$run_after" -eq 1 ]; then exec "$WHT" run "$ROOT/build/spire.wsb"; fi
