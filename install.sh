#!/bin/sh
# ---------------------------------------------------------------------------
# SPACERIP installer for the distribution (deb/rpm) version of VLC
#
#   ./install.sh              install the plugin system wide (needs sudo)
#   ./install.sh --user       install only for the current user
#   ./install.sh --uninstall  remove the plugin
# ---------------------------------------------------------------------------
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src="$here/dist/libspacerip_plugin.so"
name="libspacerip_plugin.so"

if [ ! -f "$src" ]; then
    echo "SPACERIP: cannot find $src" >&2
    exit 1
fi

mode=system
case "${1:-}" in
    --user) mode=user ;;
    --uninstall) mode=uninstall ;;
    "") ;;
    *) echo "usage: $0 [--user|--uninstall]" >&2; exit 1 ;;
esac

# --- locate the plugin directory of the installed VLC -----------------------
find_plugins () {
    for d in \
        /usr/lib/x86_64-linux-gnu/vlc/plugins \
        /usr/lib/aarch64-linux-gnu/vlc/plugins \
        /usr/lib/vlc/plugins \
        /usr/lib64/vlc/plugins
    do
        if [ -d "$d" ]; then echo "$d"; return 0; fi
    done
    return 1
}

user_dir="${XDG_DATA_HOME:-$HOME/.local/share}/vlc/plugins"

if [ "$mode" = uninstall ]; then
    removed=0
    for d in $(find_plugins || true) "$user_dir"; do
        if [ -f "$d/visualisation/$name" ]; then
            if [ -w "$d/visualisation" ]; then
                rm -f "$d/visualisation/$name"
            else
                sudo rm -f "$d/visualisation/$name"
            fi
            echo "SPACERIP: removed from $d/visualisation"
            removed=1
        fi
    done
    [ "$removed" = 1 ] || echo "SPACERIP: nothing to remove"
    exit 0
fi

if [ "$mode" = user ]; then
    mkdir -p "$user_dir/visualisation"
    cp "$src" "$user_dir/visualisation/$name"
    echo "SPACERIP: installed in $user_dir/visualisation"
    echo
    echo "VLC does not scan this directory by default: start it with"
    echo
    echo "    VLC_PLUGIN_PATH=\"$user_dir\" vlc --audio-visual=spacerip"
    echo
    echo "or add 'VLC_PLUGIN_PATH=\"$user_dir\"' to your ~/.profile and"
    echo "select SPACERIP in Audio > Effects > Visualizations."
    exit 0
fi

plugins=$(find_plugins) || {
    echo "SPACERIP: no VLC plugin directory found - is VLC installed?" >&2
    echo "You can still install for your user only:  $0 --user" >&2
    exit 1
}

dest="$plugins/visualisation"
if [ -d "$dest" ] && [ -w "$dest" ]; then
    cp "$src" "$dest/$name"
else
    echo "SPACERIP: $dest needs administrator rights (sudo)"
    sudo mkdir -p "$dest"
    sudo cp "$src" "$dest/$name"
fi

echo "SPACERIP: installed in $dest"
echo "Start VLC, then choose  Audio > Effects > Visualizations > Visualization:"
echo "and select \"SPACERIP\" (or run:  vlc --audio-visual=spacerip)."
