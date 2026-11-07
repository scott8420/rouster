#!/usr/bin/env bash
# Rouster install.
#
#   ./install.sh                 install to ~/.local (no sudo)
#   ./install.sh --system        install to /usr/local (sudo)
#   ./install.sh --prefix DIR    install to DIR
#   ./install.sh --build         run ./build.sh first
#   ./install.sh --helper        ALSO install the root helper + its polkit
#                                action (sudo; always /usr/local/libexec and
#                                /usr/share/polkit-1 -- s013). Alone it is
#                                the helper only: ./install.sh --helper
#
# Installs the binary, the .desktop file, the scalable + symbolic hicolor icons,
# and share/rouster/install-manifest.txt listing every path written, which
# uninstall.sh removes exactly. sudo is used only if the prefix isn't writable.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

APP=rouster
APP_ID=io.github.scott8420.Rouster
PREFIX="$HOME/.local"
DO_BUILD=0
DO_HELPER=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --system) PREFIX=/usr/local ;;
        --prefix) shift; PREFIX="${1:?--prefix needs a directory}" ;;
        --build)  DO_BUILD=1 ;;
        --helper) DO_HELPER=1 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

[[ $DO_BUILD -eq 1 ]] && ./build.sh

# -- The root helper (s013): fixed paths, because the polkit action names the
# program by its exact path. Owned by root, 0755; its own manifest.
if [[ $DO_HELPER -eq 1 ]]; then
    HELPER=/usr/local/libexec/rouster/rouster-helper
    POLICY=/usr/share/polkit-1/actions/$APP_ID.policy
    HMANIFEST=/usr/local/share/rouster/helper-manifest.txt
    if [[ ! -x build/rouster-helper ]]; then
        echo "install.sh: build/rouster-helper not found -- run ./build.sh (or pass --build)" >&2
        exit 1
    fi
    sudo install -D -o root -g root -m 755 build/rouster-helper "$HELPER"
    sudo install -D -o root -g root -m 644 "data/$APP_ID.policy" "$POLICY"
    hm_tmp="$(mktemp)"
    printf '%s\n' "$HELPER" "$POLICY" "$HMANIFEST" > "$hm_tmp"
    sudo install -D -o root -g root -m 644 "$hm_tmp" "$HMANIFEST"
    rm -f "$hm_tmp"
    echo "Installed Rouster's root helper"
    echo "  $HELPER"
    echo "  $POLICY"
    # --helper alone: the app is not reinstalled.
    [[ -x build/$APP && -f "$PREFIX/share/$APP/install-manifest.txt" ]] || exit 0
fi
if [[ ! -x build/$APP ]]; then
    echo "install.sh: build/$APP not found -- run ./build.sh (or pass --build)" >&2
    exit 1
fi

SUDO=()
probe="$PREFIX"
while [[ ! -e "$probe" ]]; do probe="$(dirname "$probe")"; done
if [[ ! -w "$probe" ]]; then SUDO=(sudo); fi

BINDIR="$PREFIX/bin"
APPDIR="$PREFIX/share/applications"
ICONDIR="$PREFIX/share/icons/hicolor"
SHAREDIR="$PREFIX/share/$APP"
MANIFEST="$SHAREDIR/install-manifest.txt"

written=()
put() {  # put SRC DEST MODE
    "${SUDO[@]}" install -D -m "$3" "$1" "$2"
    written+=("$2")
}

tmp_desktop="$(mktemp)"
trap 'rm -f "$tmp_desktop"' EXIT
sed "s|@BINDIR@|$BINDIR|g" "data/$APP_ID.desktop.in" > "$tmp_desktop"

put "build/$APP"                                       "$BINDIR/$APP"                                         755
put "$tmp_desktop"                                     "$APPDIR/$APP_ID.desktop"                              644
put "data/$APP_ID.svg"                                 "$ICONDIR/scalable/apps/$APP_ID.svg"                   644
put "resources/icons/scalable/apps/$APP-logo-symbolic.svg" "$ICONDIR/symbolic/apps/$APP_ID-symbolic.svg"      644

manifest_tmp="$(mktemp)"
printf '%s\n' "${written[@]}" "$MANIFEST" > "$manifest_tmp"
"${SUDO[@]}" install -D -m 644 "$manifest_tmp" "$MANIFEST"
rm -f "$manifest_tmp"

command -v gtk-update-icon-cache   >/dev/null && "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$ICONDIR" || true
command -v update-desktop-database >/dev/null && "${SUDO[@]}" update-desktop-database -q "$APPDIR" || true

echo "Installed Rouster to $PREFIX"
echo "  manifest: $MANIFEST"
case ":$PATH:" in *":$BINDIR:"*) ;; *) echo "  note: $BINDIR is not on your PATH" ;; esac
