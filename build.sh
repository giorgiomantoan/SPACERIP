#!/bin/sh
# SPACERIP - build script (Linux/macOS, GCC or Clang)
#
#   ./build.sh                  build dist/libspacerip_plugin.so
#   VLC_VERSION=3.0.20 ./build.sh   build against another VLC 3.0.x
#   VLC_SRC=/path/to/vlc ./build.sh use an existing VLC source tree
#
# The plugin only needs the VLC *headers*; it is compiled as a shared object
# with no link-time dependency on VLC itself (symbols are resolved when VLC
# loads the module). On distributions providing the plugin headers you can
# point VLC_SRC to /usr/include/vlc instead of downloading the sources.
set -eu

cd "$(dirname "$0")"

VLC_VERSION="${VLC_VERSION:-3.0.24}"
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=gnu11 -O2 -Wall -fPIC}"
LDFLAGS="${LDFLAGS:--shared}"
OUT="dist/libspacerip_plugin.so"

if [ -n "${VLC_SRC:-}" ]; then
    INC="$VLC_SRC/include"
else
    VLC_SRC="build/vlc-$VLC_VERSION"
    INC="$VLC_SRC/include"
    if [ ! -f "$INC/vlc_plugin.h" ]; then
        echo "==> fetching VLC $VLC_VERSION headers"
        mkdir -p build
        TARBALL="build/vlc-$VLC_VERSION.tar.xz"
        if [ ! -f "$TARBALL" ]; then
            wget -O "$TARBALL" \
                "https://download.videolan.org/pub/videolan/vlc/$VLC_VERSION/vlc-$VLC_VERSION.tar.xz" \
                || curl -L -o "$TARBALL" \
                "https://download.videolan.org/pub/videolan/vlc/$VLC_VERSION/vlc-$VLC_VERSION.tar.xz"
        fi
        tar -xf "$TARBALL" -C build
    fi
fi

if [ ! -f "$INC/vlc_plugin.h" ]; then
    echo "error: vlc_plugin.h not found in $INC" >&2
    exit 1
fi

mkdir -p dist

echo "==> compiling spacerip.c"
# shellcheck disable=SC2086
$CC $CFLAGS $LDFLAGS \
    -D__PLUGIN__ -DMODULE_STRING='"spacerip"' \
    -I"$INC" \
    src/spacerip.c -o "$OUT" \
    ${LIBS:--lm -ldl -lpthread}

echo "==> built $OUT"
if command -v file >/dev/null 2>&1; then file "$OUT"; fi
