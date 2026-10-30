#!/usr/bin/env bash
# Applies the saved defconfig to a fresh Buildroot output directory.
#   scripts/configure.sh          apply difiw_x86_64_defconfig
#   scripts/configure.sh --save   write the current Buildroot config back as a minimal defconfig
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
"$PROJECT_ROOT/scripts/fetch-buildroot.sh"
"$PROJECT_ROOT/scripts/sync.sh"

br_make() {
    make -C "$BR_DIR" O="$BR_OUT" BR2_EXTERNAL="$MIRROR/buildroot-external" BR2_DL_DIR="$BR_DL" "$@"
}

if [ "${1:-}" = "--save" ]; then
    br_make savedefconfig BR2_DEFCONFIG="$PROJECT_ROOT/buildroot-external/configs/difiw_x86_64_defconfig"
    log "defconfig saved to buildroot-external/configs/difiw_x86_64_defconfig"
    exit 0
fi

log "applying difiw_x86_64_defconfig"
br_make difiw_x86_64_defconfig
