#!/usr/bin/env bash
# Mirrors the project sources into the Linux build tree (ASCII path, real permissions).
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
init_build_root
mkdir -p "$MIRROR"
rsync -a --delete \
    --exclude '.git/' --exclude 'artifacts/' --exclude 'build/' --exclude '*.log' \
    "$PROJECT_ROOT/" "$MIRROR/"
# Windows checkouts may lose the executable bit; restore it where it matters.
chmod +x "$MIRROR"/scripts/*.sh "$MIRROR"/tests/*.sh \
    "$MIRROR"/buildroot-external/board/difiw/x86_64/*.sh \
    "$MIRROR"/buildroot-external/board/difiw/x86_64/overlay/etc/init.d/* \
    "$MIRROR"/buildroot-external/board/difiw/x86_64/overlay/usr/libexec/*
# Normalize line endings in case the files were checked out with CRLF.
find "$MIRROR/scripts" "$MIRROR/tests" "$MIRROR/buildroot-external" "$MIRROR/src" \
    -type f \( -name '*.sh' -o -name '*.mk' -o -name '*.cfg' -o -name '*.fragment' -o -name 'Config.in' \
    -o -name 'external.*' -o -name '*defconfig' -o -name 'inittab' -o -name 'S10*' -o -name 'difiw-session' \
    -o -name '*.cpp' -o -name '*.hpp' -o -name 'Makefile' -o -name '*.py' \) \
    -exec sed -i 's/\r$//' {} +
log "project mirrored to $MIRROR"
