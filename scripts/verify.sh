#!/usr/bin/env bash
# Verifies the build: host unit tests, ISO structure, kernel config, QEMU boot test.
#   scripts/verify.sh            everything
#   scripts/verify.sh --no-qemu  skip the QEMU boot test
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
init_build_root
REPORT="$ARTIFACTS/verify-report.log"
mkdir -p "$ARTIFACTS"
: > "$REPORT"
exec > >(tee -a "$REPORT") 2>&1

ISO="$ARTIFACTS/$ISO_NAME"
fail=0
ok()   { echo "ok   - $*"; }
bad()  { echo "FAIL - $*"; fail=1; }

echo "== Difiw OS $DIFIW_VERSION verification, $(date -u +%FT%TZ) =="

echo "-- version consistency"
grep -q "kVersion = \"$DIFIW_VERSION\"" "$PROJECT_ROOT/src/difiw-shell/commands.hpp" \
    && ok "VERSION matches Difiw Shell" || bad "VERSION differs from src/difiw-shell/commands.hpp"

echo "-- host unit + smoke tests of Difiw Shell"
T="$(mktemp -d)"; trap 'rm -rf -- "$T"' EXIT
cp -r "$PROJECT_ROOT/src" "$PROJECT_ROOT/tests" "$T/"
( cd "$T/src/difiw-shell" && make -s test && make -s ) >"$T/host.log" 2>&1 \
    && ok "unit tests + host build" || { bad "host build/unit tests"; cat "$T/host.log"; }
bash "$PROJECT_ROOT/tests/shell_smoke.sh" "$T/src/difiw-shell/build/difiw-shell" | tail -3

echo "-- ISO"
if [ -s "$ISO" ]; then
    size="$(stat -c %s "$ISO")"
    [ "$size" -gt 5000000 ] && [ "$size" -lt 500000000 ] && ok "ISO size $size bytes" || bad "ISO size out of range: $size"
    file -b "$ISO"
    ( cd "$ARTIFACTS" && sha256sum --check --status SHA256SUMS ) && ok "SHA256SUMS matches" || bad "SHA256SUMS mismatch"
    listing="$(xorriso -indev "$ISO" -find 2>/dev/null || true)"
    for f in /boot/bzImage /boot/rootfs.cpio.gz /boot/grub/grub.cfg; do
        grep -q "'\.$f'" <<<"$listing" && ok "ISO contains $f" || bad "ISO lacks $f"
    done
    xorriso -indev "$ISO" -report_el_torito plain 2>/dev/null | grep -q 'El Torito' \
        && ok "El Torito boot record present" || bad "no El Torito boot record"
else
    bad "ISO missing: $ISO"
fi

echo "-- kernel configuration"
KCFG="$(ls "$BR_OUT"/build/linux-*/.config 2>/dev/null | head -1 || true)"
if [ -n "$KCFG" ]; then
    for s in CONFIG_BINFMT_ELF CONFIG_BLK_DEV_INITRD CONFIG_DEVTMPFS CONFIG_TMPFS CONFIG_PROC_FS CONFIG_SYSFS \
             CONFIG_VT_CONSOLE CONFIG_VGA_CONSOLE CONFIG_SERIAL_8250_CONSOLE CONFIG_PCI CONFIG_ACPI CONFIG_FB_VESA CONFIG_FRAMEBUFFER_CONSOLE CONFIG_INPUT_EVDEV CONFIG_USB_HID \
             CONFIG_RTC_DRV_CMOS CONFIG_SATA_AHCI CONFIG_EXT4_FS CONFIG_VIRTIO_BLK CONFIG_KEYBOARD_ATKBD; do
        grep -q "^$s=y" "$KCFG" && ok "$s=y" || bad "$s not built in"
    done
    grep -q '^CONFIG_LOCALVERSION="-difiw"' "$KCFG" && ok "localversion -difiw" || bad "localversion"
else
    bad "kernel .config not found (build first)"
fi

if [ "${1:-}" != "--no-qemu" ]; then
    echo "-- QEMU boot test"
    command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not installed"
    python3 -I "$PROJECT_ROOT/tests/smoke_qemu.py" "$ISO" "$ARTIFACTS/qemu-serial.log" && ok "QEMU test" || bad "QEMU test"
    echo "-- QEMU graphical shell test (screen, mouse, keyboard, save, terminal)"
    # not -I: the test imports its sibling smoke_qemu.py
    python3 "$PROJECT_ROOT/tests/gui_qemu.py" "$ISO" "$ARTIFACTS/qemu-gui-serial.log" "$T/shots" && ok "QEMU GUI test" || bad "QEMU GUI test"
fi

echo
[ "$fail" -eq 0 ] && echo "VERIFY: PASSED" || { echo "VERIFY: FAILED"; exit 1; }
