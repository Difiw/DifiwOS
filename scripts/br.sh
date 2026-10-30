#!/usr/bin/env bash
# Runs any Buildroot target against the Difiw build tree, e.g.:
#   scripts/br.sh kbd-rebuild      scripts/br.sh menuconfig
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
"$PROJECT_ROOT/scripts/sync.sh" >/dev/null
exec make -C "$BR_DIR" O="$BR_OUT" BR2_EXTERNAL="$MIRROR/buildroot-external" BR2_DL_DIR="$BR_DL" -j"$(jobs_count)" "$@"
