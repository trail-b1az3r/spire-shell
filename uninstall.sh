#!/usr/bin/env bash
# uninstall.sh - removes the spire binary. Config/history are kept unless
# you pass --purge.
set -euo pipefail

PREFIX="/usr/local"
PURGE=0
for arg in "$@"; do
    case "$arg" in
        --prefix=*) PREFIX="${arg#--prefix=}" ;;
        --user) PREFIX="$HOME/.local" ;;
        --purge) PURGE=1 ;;
        -h|--help) echo "Usage: ./uninstall.sh [--prefix=DIR] [--user] [--purge]"; exit 0 ;;
        *) echo "uninstall.sh: unknown option '$arg'" >&2; exit 1 ;;
    esac
done

BIN="$PREFIX/bin/spire"
if [ -w "$(dirname "$BIN")" ] 2>/dev/null; then
    rm -f "$BIN"
else
    sudo rm -f "$BIN"
fi
echo "removed $BIN"

if grep -qxF "$BIN" /etc/shells 2>/dev/null; then
    if [ -w /etc/shells ]; then
        sed -i.bak "\#^$BIN\$#d" /etc/shells
    elif command -v sudo >/dev/null 2>&1; then
        sudo sed -i.bak "\#^$BIN\$#d" /etc/shells
    fi
    echo "removed $BIN from /etc/shells"
fi

if [ "$PURGE" -eq 1 ]; then
    rm -rf "$HOME/.config/spire" "$HOME/.local/share/spire"
    echo "removed configuration and history"
else
    echo "left ~/.config/spire and ~/.local/share/spire in place (use --purge to remove them)"
fi
