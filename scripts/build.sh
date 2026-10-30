#!/usr/bin/env bash
# Full build: fetch Buildroot, configure, compile toolchain/kernel/rootfs, make the ISO,
# copy it with SHA256SUMS to artifacts/.
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
"$PROJECT_ROOT/scripts/check-host.sh"
"$PROJECT_ROOT/scripts/configure.sh"

JOBS="$(jobs_count)"
LOG="$BUILD_ROOT/build.log"
log "building with $JOBS parallel jobs; full log: $LOG"
log "first build takes a long time (toolchain + kernel); later runs are incremental"

# Overlay, post-build and post-image steps are re-run by Buildroot on every make.
set +e
make -C "$BR_DIR" O="$BR_OUT" BR2_EXTERNAL="$MIRROR/buildroot-external" BR2_DL_DIR="$BR_DL" \
    -j"$JOBS" 2>&1 | tee "$LOG"
rc="${PIPESTATUS[0]}"
set -e
[ "$rc" -eq 0 ] || die "Buildroot make failed with status $rc (see $LOG)"

ISO="$BR_OUT/images/$ISO_NAME"
[ -s "$ISO" ] || die "ISO was not produced: $ISO"

mkdir -p "$ARTIFACTS"
cp -- "$ISO" "$ARTIFACTS/$ISO_NAME"
( cd "$ARTIFACTS" && sha256sum "$ISO_NAME" > SHA256SUMS )

KREL="$(cat "$BR_OUT"/build/linux-*/include/config/kernel.release 2>/dev/null | head -1 || true)"
{
    echo "Difiw OS $DIFIW_VERSION Core build report"
    echo "date:        $(date -u +%FT%TZ)"
    echo "buildroot:   $BUILDROOT_VERSION"
    echo "kernel:      ${KREL:-unknown}"
    echo "iso:         $ISO_NAME ($(stat -c %s "$ARTIFACTS/$ISO_NAME") bytes)"
    echo "sha256:      $(cut -d' ' -f1 "$ARTIFACTS/SHA256SUMS")"
} > "$ARTIFACTS/build-report.txt"
cat "$ARTIFACTS/build-report.txt"
log "done: $ARTIFACTS/$ISO_NAME"
