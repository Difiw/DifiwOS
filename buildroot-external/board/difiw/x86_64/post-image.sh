#!/usr/bin/env bash
# Buildroot post-image: builds the bootable BIOS ISO with GRUB 2 from this build's
# kernel (bzImage) and rootfs (initramfs cpio). Called as: post-image.sh <images dir>
# SPDX-License-Identifier: MIT
set -euo pipefail

IMAGES_DIR="${1:?images dir missing}"
BOARD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(tr -d '[:space:]' < "$BOARD_DIR/../../../../VERSION")"
ISO="$IMAGES_DIR/difiw-os-${VERSION}-x86_64.iso"
STAGE="${BASE_DIR:?BASE_DIR not set}/iso-root"

for f in bzImage rootfs.cpio.gz; do
    [ -s "$IMAGES_DIR/$f" ] || { echo "post-image: missing $IMAGES_DIR/$f" >&2; exit 1; }
done
command -v grub-mkrescue >/dev/null || { echo "post-image: grub-mkrescue not found (install grub-pc-bin, xorriso, mtools)" >&2; exit 1; }

# STAGE is $BASE_DIR/iso-root, created by this script only.
rm -rf -- "${STAGE:?}"
mkdir -p "$STAGE/boot/grub"
cp "$IMAGES_DIR/bzImage" "$STAGE/boot/bzImage"
cp "$IMAGES_DIR/rootfs.cpio.gz" "$STAGE/boot/rootfs.cpio.gz"
cp "$BOARD_DIR/grub.cfg" "$STAGE/boot/grub/grub.cfg"

rm -f -- "$ISO"
grub-mkrescue --product-name "Difiw OS" -o "$ISO" "$STAGE" -- -volid DIFIW_OS
echo "post-image: created $ISO"
