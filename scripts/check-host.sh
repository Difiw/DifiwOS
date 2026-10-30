#!/usr/bin/env bash
# Audits the build host: OS, resources, required tools.
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
missing=0

echo "== Host =="
echo "Kernel : $(uname -sr) ($(uname -m))"
[ -r /etc/os-release ] && . /etc/os-release && echo "Distro : ${PRETTY_NAME:-unknown}"
grep -qi microsoft /proc/version && echo "WSL    : yes"
echo "CPUs   : $(nproc)"
echo "RAM    : $(awk '/MemTotal/ {printf "%.1f GiB", $2/1048576}' /proc/meminfo)"
mkdir -p "$BUILD_ROOT"
echo "Build  : $BUILD_ROOT ($(df -BG --output=avail "$BUILD_ROOT" | tail -1 | tr -d ' ') free)"
avail_gb="$(df -BG --output=avail "$BUILD_ROOT" | tail -1 | tr -dc '0-9')"
[ "$avail_gb" -ge 20 ] || { warn "less than 20 GiB free in $BUILD_ROOT"; missing=1; }
case "$BUILD_ROOT" in /mnt/*) warn "build root is on a Windows drive; use a Linux path (default: ~/difiw-build)";; esac
case "$BUILD_ROOT" in *[!A-Za-z0-9_./-]*) warn "build root contains unusual characters; use an ASCII path"; missing=1;; esac

echo; echo "== Tools =="
for t in gcc g++ make git perl python3 tar gzip bzip2 xz rsync cpio patch bc wget curl file which sed awk \
         unzip xorriso grub-mkrescue mformat qemu-system-x86_64 flex bison sha256sum; do
    if p="$(command -v "$t" 2>/dev/null)"; then printf '  ok       %-22s %s\n' "$t" "$p"
    else printf '  MISSING  %s\n' "$t"; missing=1; fi
done
if [ -d /usr/lib/grub/i386-pc ]; then echo "  ok       grub i386-pc modules"
else echo "  MISSING  grub-pc-bin (/usr/lib/grub/i386-pc)"; missing=1; fi
for h in libssl-dev libelf-dev libncurses-dev; do
    dpkg -s "$h" >/dev/null 2>&1 && echo "  ok       $h" || { echo "  MISSING  $h"; missing=1; }
done
command -v VBoxManage >/dev/null 2>&1 || command -v VBoxManage.exe >/dev/null 2>&1 \
    && echo "  ok       VirtualBox CLI (optional)" || echo "  absent   VirtualBox CLI in this shell (optional)"

if [ "$missing" -ne 0 ]; then
    echo
    echo "Install on Ubuntu/Debian:"
    echo "  sudo apt-get install -y build-essential git wget curl rsync cpio bc perl python3 unzip file xz-utils bzip2 patch \\"
    echo "       libncurses-dev libssl-dev libelf-dev flex bison xorriso grub-pc-bin grub-common mtools qemu-system-x86 e2fsprogs"
    die "host check failed"
fi
log "host check passed"
