#!/bin/sh
# ---------------------------------------------------------------------------
# SPACERIP launcher for the VLC snap
#
#   spacerip-vlc.sh [VLC options] file1 [file2 ...]
#
# Starts the VLC snap with the SPACERIP interstellar visualisation already
# active.  Every argument is forwarded to VLC, for example:
#
#   spacerip-vlc.sh "my song.mp3"
#   spacerip-vlc.sh --spacerip-fullscreen concert.flac
#
# The SPACERIP window follows the rhythm of the music: warp speed on the
# beats, nebulae on the bass, star trails on the melody.
# ---------------------------------------------------------------------------
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
plug="$here/dist"

if [ ! -f "$plug/libspacerip_plugin.so" ]; then
    echo "SPACERIP: cannot find $plug/libspacerip_plugin.so" >&2
    echo "Keep spacerip-vlc.sh next to the dist/ directory of the release." >&2
    exit 1
fi

if ! snap run --shell vlc -c 'true' >/dev/null 2>&1; then
    echo "SPACERIP: this launcher needs the VLC snap." >&2
    echo "Install it with:  sudo snap install vlc" >&2
    exit 1
fi

# quote every argument so that it survives the two shell layers
args=""
for a in "$@"; do
    q=$(printf '%s' "$a" | sed "s/'/'\\\\''/g")
    args="$args '$q'"
done

exec snap run --shell vlc -c "DISPLAY=\${DISPLAY:-:0} \
LD_LIBRARY_PATH=\"\$SNAP/usr/lib:\$SNAP/usr/lib/vlc:\${LD_LIBRARY_PATH:-}\" \
VLC_PLUGIN_PATH='$plug' \
QT_QPA_PLATFORM=xcb \
exec vlc --no-one-instance --intf=qt --audio-visual=spacerip $args < /dev/null"
