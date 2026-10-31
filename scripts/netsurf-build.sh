#!/usr/bin/env bash
# Cross-builds the NetSurf web engine (framebuffer front end + libnsfb "difiw" surface) with the Buildroot toolchain.
# Called from buildroot-external/package/difiw-netsurf; can also be run by hand inside WSL after a Buildroot build.
#   scripts/netsurf-build.sh <dest-dir>      installs the browser tree into <dest-dir> (usr/bin/netsurf-fb, usr/share/netsurf)
# SPDX-License-Identifier: MIT
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${DIFIW_BUILD_ROOT:-$HOME/difiw-build}"
BR_OUT="${BR_OUT:-$BUILD_ROOT/output}"
NS_VER=3.11
NS_URL="https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-$NS_VER.tar.gz"
NS_SHA256="${NS_SHA256:-}"
DEST="$(realpath -m "${1:?destination directory}")"
WORK="$BUILD_ROOT/netsurf-work"
TRIPLE="$(ls "$BR_OUT/host/bin" | sed -n 's/-gcc$//p' | grep -- '-buildroot-' | head -1)"
SYSROOT="$BR_OUT/host/$TRIPLE/sysroot"
export PATH="$BR_OUT/host/bin:/usr/bin:/bin"

mkdir -p "$WORK" "$BUILD_ROOT/dl"
TAR="$BUILD_ROOT/dl/netsurf-all-$NS_VER.tar.gz"
[ -s "$TAR" ] || curl -fsSL -o "$TAR" "$NS_URL"
[ -z "$NS_SHA256" ] || echo "$NS_SHA256  $TAR" | sha256sum -c -
rm -rf "$WORK/src"; mkdir -p "$WORK/src"
tar -xzf "$TAR" -C "$WORK/src" --strip-components=1
cd "$WORK/src"

# --- Difiw patches --------------------------------------------------------------------------
cp "$PROJECT_ROOT/src/netsurf/difiw_surface.c" libnsfb/src/surface/ram.c
# plain Unicode characters from the desktop travel as keycode 0x100000 + code point
perl -0pi -e 's/(fbtk_keycode_to_ucs4\(int code, fbtk_modifier_type mods\)\n\{\n\tint ucs4 = -1;\n)/$1\n\tif (code >= 0x100000)\n\t\treturn code - 0x100000;\n/' netsurf/frontends/framebuffer/fbtk/event.c
grep -q 0x100000 netsurf/frontends/framebuffer/fbtk/event.c
[ ! -f "$PROJECT_ROOT/src/netsurf/netsurf.patch" ] || patch -p1 < "$PROJECT_ROOT/src/netsurf/netsurf.patch"

cat > netsurf/Makefile.config <<CFG
override NETSURF_USE_DUKTAPE := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_FB_FONTLIB := freetype
override NETSURF_FB_FONTPATH := /usr/share/fonts/dejavu
override NETSURF_FB_FONT_CURSIVE := DejaVuSans.ttf
override NETSURF_FB_FONT_FANTASY := DejaVuSans.ttf
override NETSURF_FB_RESPATH := /usr/share/netsurf/
CFG

# --- cross environment ------------------------------------------------------------------------
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
NSINST="$SYSROOT/opt/netsurf-inst"
rm -rf "$NSINST"
export PKG_CONFIG_PATH="$NSINST/lib/pkgconfig"
export CC="$TRIPLE-gcc" AR="$TRIPLE-ar" LD="$TRIPLE-ld" RANLIB="$TRIPLE-ranlib" STRIP="$TRIPLE-strip"
export BUILD_CC=gcc
export CFLAGS="--sysroot=$SYSROOT -O2 -I$SYSROOT/usr/include"
export LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib"
BUILD_TRIPLE="$(gcc -dumpmachine)"

make -j"$(nproc)" TMP_PREFIX="$NSINST" TARGET=framebuffer HOST="$TRIPLE" BUILD="$BUILD_TRIPLE" PREFIX=/usr \
     NETSURF_CONFIG="NETSURF_USE_DUKTAPE=NO" build 2>&1 | tee "$WORK/build.log"

mkdir -p "$DEST/usr/bin" "$DEST/usr/share/netsurf"
cp netsurf/nsfb "$DEST/usr/bin/netsurf-fb" 2>/dev/null || cp netsurf/nsfb* "$DEST/usr/bin/netsurf-fb"
"$STRIP" "$DEST/usr/bin/netsurf-fb"
cp -r netsurf/frontends/framebuffer/res/. "$DEST/usr/share/netsurf/"
echo "NetSurf installed into $DEST"
