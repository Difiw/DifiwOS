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

chmod 0755 "$TARGET_DIR/etc/init.d/S10difiw-boot" "$TARGET_DIR/usr/libexec/difiw-session" "$TARGET_DIR/usr/libexec/difiw-console"
chmod 0644 "$TARGET_DIR/etc/inittab"
