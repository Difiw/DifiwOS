#!/usr/bin/env bash
# Downloads Buildroot from buildroot.org, verifies the pinned SHA-256, unpacks it.
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

require_linux
init_build_root

TARBALL="$BR_DL/buildroot-$BUILDROOT_VERSION.tar.xz"

verify() { echo "$BUILDROOT_SHA256  $1" | sha256sum --check --status; }

if [ -f "$TARBALL" ] && verify "$TARBALL"; then
    log "Buildroot tarball already present and verified"
else
    log "downloading $BUILDROOT_URL"
    curl -fL --retry 3 -o "$TARBALL.part" "$BUILDROOT_URL"
    verify "$TARBALL.part" || { rm -f -- "$TARBALL.part"; die "SHA-256 mismatch for Buildroot tarball"; }
    mv -- "$TARBALL.part" "$TARBALL"
    log "checksum OK"
fi

if [ -f "$BR_DIR/Makefile" ]; then
    log "Buildroot already unpacked: $BR_DIR"
else
    log "unpacking into $BUILD_ROOT"
    tar -C "$BUILD_ROOT" -xf "$TARBALL"
    [ -f "$BR_DIR/Makefile" ] || die "unexpected tarball layout"
fi
