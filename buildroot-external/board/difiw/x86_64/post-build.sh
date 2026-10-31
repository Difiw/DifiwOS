#!/usr/bin/env bash
# Buildroot post-build: branding and permissions inside the target rootfs.
# Called as: post-build.sh <target dir>
# SPDX-License-Identifier: MIT
set -euo pipefail

TARGET_DIR="${1:?target dir missing}"
BOARD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(tr -d '[:space:]' < "$BOARD_DIR/../../../../VERSION")"

# os-release (the /etc/os-release symlink points here)
install -d "$TARGET_DIR/usr/lib"
cat > "$TARGET_DIR/usr/lib/os-release" <<EOF
NAME="Difiw OS"
VERSION="${VERSION} Core"
VERSION_ID=${VERSION}
ID=difiw
PRETTY_NAME="Difiw OS ${VERSION} Core"
HOME_URL="https://example.invalid/difiw-os"
EOF
ln -sf ../usr/lib/os-release "$TARGET_DIR/etc/os-release"

printf '/bin/sh\n/usr/bin/difiw-shell\n' > "$TARGET_DIR/etc/shells"

# kbd ships ~3 MB of keymaps/fonts; keep only what the `en` / `ru` commands load.
SHARE="$TARGET_DIR/usr/share"
if [ -d "$SHARE/keymaps" ]; then
    find "$SHARE/keymaps" -type f ! -path '*/include/*' ! -name 'us.map.gz' ! -name 'ruwin_alt_sh-UTF-8.map.gz' -delete
    find "$SHARE/keymaps" -type d -empty -delete
fi
if [ -d "$SHARE/consolefonts" ]; then
    find "$SHARE/consolefonts" -type f ! -name 'cyr-sun16.psfu.gz' -delete
fi
rm -rf "$SHARE/consoletrans" "$SHARE/man"
# Incremental builds keep the trimmed target; restore needed files from the kbd source tree.
KBD_DATA="$(ls -d "${BUILD_DIR:?BUILD_DIR not set}"/kbd-*/data 2>/dev/null | head -1 || true)"
for f in keymaps/i386/qwerty/us.map.gz keymaps/i386/qwerty/ruwin_alt_sh-UTF-8.map.gz consolefonts/cyr-sun16.psfu.gz; do
    src="${f%%/*}_Z/${f#*/}"   # kbd keeps compressed copies in keymaps_Z/, consolefonts_Z/
    if [ ! -f "$SHARE/$f" ] && [ -n "$KBD_DATA" ] && [ -f "$KBD_DATA/$src" ]; then
        install -D -m 0644 "$KBD_DATA/$src" "$SHARE/$f"
    fi
done
[ -f "$SHARE/keymaps/i386/qwerty/us.map.gz" ] || { echo "post-build: us keymap missing" >&2; exit 1; }
[ -f "$SHARE/consolefonts/cyr-sun16.psfu.gz" ] || { echo "post-build: cyrillic font missing" >&2; exit 1; }
[ -f "$SHARE/keymaps/i386/qwerty/ruwin_alt_sh-UTF-8.map.gz" ] || { echo "post-build: Russian keymap missing" >&2; exit 1; }

# The graphical shell reads an uncompressed PSF console font (Latin + Cyrillic).
install -d "$SHARE/difiw"
gzip -dc "$SHARE/consolefonts/cyr-sun16.psfu.gz" > "$SHARE/difiw/font.psf"
[ -s "$SHARE/difiw/font.psf" ] || { echo "post-build: font.psf is empty" >&2; exit 1; }

rm -rf "$TARGET_DIR/usr/share/difiw/ru.kmap"
# default wallpaper (1280x720 PPM) unpacked from the repository asset
gzip -dc "$BOARD_DIR/../../../../assets/wallpaper.ppm.gz" > "$SHARE/difiw/wallpaper.ppm"
[ -s "$SHARE/difiw/wallpaper.ppm" ] || { echo "post-build: wallpaper.ppm is empty" >&2; exit 1; }
chmod 0644 "$SHARE/difiw/font.psf" "$SHARE/difiw/wallpaper.ppm"
chmod 0755 "$TARGET_DIR/etc/difiw" "$SHARE/difiw/apps" "$TARGET_DIR/etc/difiw/services.d"
install -d "$SHARE/difiw/icons"
chmod 0755 "$SHARE/difiw/icons"
chmod 0644 "$SHARE"/difiw/icons/*
chmod 0644 "$SHARE"/difiw/apps/*.app "$TARGET_DIR"/etc/difiw/services.d/*.svc
chmod 0755 "$TARGET_DIR/etc/init.d/S10difiw-boot" "$TARGET_DIR/etc/init.d/S41difiw-net" "$TARGET_DIR/etc/init.d/S42difiw-sound" "$TARGET_DIR/usr/libexec/difiw-session" "$TARGET_DIR/usr/libexec/difiw-console"
chmod 0755 "$TARGET_DIR/etc/difiw/udhcpc.script"
install -d "$SHARE/difiw/sounds"
chmod 0644 "$SHARE"/difiw/sounds/*
chmod 0644 "$TARGET_DIR/etc/inittab"
