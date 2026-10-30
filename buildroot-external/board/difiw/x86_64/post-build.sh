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

chmod 0755 "$TARGET_DIR/etc/init.d/S10difiw-boot" "$TARGET_DIR/usr/libexec/difiw-session"
chmod 0644 "$TARGET_DIR/etc/inittab"
