#!/usr/bin/env bash
# install.sh - build and install spire.
#
# Usage:
#   ./install.sh                 # build, install to /usr/local/bin (sudo if needed)
#   ./install.sh --prefix=DIR    # install into DIR/bin instead
#   ./install.sh --user          # install to ~/.local/bin, no sudo, no /etc/shells edit
#   ./install.sh --no-shells     # skip registering spire in /etc/shells
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PREFIX="/usr/local"
USER_INSTALL=0
EDIT_SHELLS=1

for arg in "$@"; do
    case "$arg" in
        --prefix=*) PREFIX="${arg#--prefix=}" ;;
        --user) USER_INSTALL=1; EDIT_SHELLS=0 ;;
        --no-shells) EDIT_SHELLS=0 ;;
        -h|--help)
            echo "Usage: ./install.sh [--prefix=DIR] [--user] [--no-shells]"
            exit 0
            ;;
        *) echo "install.sh: unknown option '$arg'" >&2; exit 1 ;;
    esac
done

if [ "$USER_INSTALL" -eq 1 ]; then
    PREFIX="$HOME/.local"
fi

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
info() { printf '  \033[36m->\033[0m %s\n' "$1"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$1"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$1"; }

bold "spire — building"

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    echo "error: no C compiler found (need cc, gcc, or clang)." >&2
    exit 1
fi
if ! command -v make >/dev/null 2>&1; then
    echo "error: 'make' not found." >&2
    exit 1
fi

make clean >/dev/null 2>&1 || true
make
ok "built ./spire"

BIN_DIR="$PREFIX/bin"
NEED_SUDO=0
if [ ! -d "$BIN_DIR" ]; then
    if ! mkdir -p "$BIN_DIR" 2>/dev/null; then NEED_SUDO=1; fi
elif [ ! -w "$BIN_DIR" ]; then
    NEED_SUDO=1
fi

bold "spire — installing to $BIN_DIR"

if [ "$NEED_SUDO" -eq 1 ]; then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "error: $BIN_DIR is not writable and 'sudo' is unavailable." >&2
        echo "       re-run with --user to install to \$HOME/.local/bin instead." >&2
        exit 1
    fi
    info "using sudo to install into $BIN_DIR"
    sudo mkdir -p "$BIN_DIR"
    sudo install -m 755 spire "$BIN_DIR/spire"
else
    mkdir -p "$BIN_DIR"
    install -m 755 spire "$BIN_DIR/spire"
fi
ok "installed $BIN_DIR/spire"

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) warn "$BIN_DIR is not on your PATH — add it to your shell's rc file" ;;
esac

bold "spire — configuration"

CFG_DIR="$HOME/.config/spire"
mkdir -p "$CFG_DIR/modules"
if [ ! -f "$CFG_DIR/spire.conf" ]; then
    cp "config/spire.conf" "$CFG_DIR/spire.conf"
    ok "wrote $CFG_DIR/spire.conf"
else
    info "$CFG_DIR/spire.conf already exists, leaving it alone"
fi
for m in config/modules/*.spire; do
    name="$(basename "$m")"
    if [ ! -f "$CFG_DIR/modules/$name" ]; then
        cp "$m" "$CFG_DIR/modules/$name"
        ok "installed module $name"
    fi
done

mkdir -p "$HOME/.local/share/spire"

SPIRE_BIN="$BIN_DIR/spire"
if [ "$EDIT_SHELLS" -eq 1 ] && [ -f /etc/shells ]; then
    if ! grep -qxF "$SPIRE_BIN" /etc/shells 2>/dev/null; then
        bold "spire — registering as a login shell"
        if [ -w /etc/shells ]; then
            echo "$SPIRE_BIN" >> /etc/shells
            ok "added $SPIRE_BIN to /etc/shells"
        elif command -v sudo >/dev/null 2>&1; then
            if sudo sh -c "echo '$SPIRE_BIN' >> /etc/shells"; then
                ok "added $SPIRE_BIN to /etc/shells"
            else
                warn "could not update /etc/shells; run: echo $SPIRE_BIN | sudo tee -a /etc/shells"
            fi
        else
            warn "could not update /etc/shells (no write access, no sudo)"
        fi
    fi
fi

echo
bold "done."
echo "  run it directly:   $SPIRE_BIN"
echo "  make it your login shell:   chsh -s $SPIRE_BIN"
echo "  edit config:        $CFG_DIR/spire.conf"
echo "  modules live in:    $CFG_DIR/modules/"
echo
