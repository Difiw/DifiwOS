#!/usr/bin/env bash
# Boots the ISO interactively in QEMU. Serial console goes to this terminal.
# Quit: Ctrl-A then X.   Optional: DATA_DISK=path/to/data.img (ext4 labelled DIFIWDATA)
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

ISO="${1:-$ARTIFACTS/$ISO_NAME}"
[ -s "$ISO" ] || die "ISO not found: $ISO (run scripts/build.sh first)"
args=(-m 512 -cdrom "$ISO" -boot d -nographic -no-reboot)
[ -w /dev/kvm ] && args+=(-accel kvm -cpu host)
[ -n "${DATA_DISK:-}" ] && args+=(-drive "file=$DATA_DISK,format=raw,if=virtio")
exec qemu-system-x86_64 "${args[@]}"
