#!/usr/bin/env bash
# Collects licenses and corresponding source of all components via Buildroot legal-info.
# Output: artifacts/legal-info/ (not a legal audit; see docs/licenses.md)
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
[ -f "$BR_OUT/.config" ] || die "run scripts/configure.sh / build.sh first"
make -C "$BR_DIR" O="$BR_OUT" BR2_EXTERNAL="$MIRROR/buildroot-external" BR2_DL_DIR="$BR_DL" legal-info
rm -rf -- "${ARTIFACTS:?}/legal-info"
mkdir -p "$ARTIFACTS"
cp -r "$BR_OUT/legal-info" "$ARTIFACTS/legal-info"
log "legal-info copied to $ARTIFACTS/legal-info"
