#!/usr/bin/env bash
# Removes build artifacts of this project only.
#   scripts/clean.sh            remove Buildroot output + project mirror (keeps downloads)
#   scripts/clean.sh --all      also remove downloads and the unpacked Buildroot
#   scripts/clean.sh --artifacts also remove artifacts/*.iso and checksums
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

safe_rm() { # refuse anything that is not inside the verified build root
    local t
    t="$(realpath -m -- "$1")"
    case "$t" in
        "$BUILD_ROOT"/*) ;;
        *) die "refusing to delete outside the build root: $t" ;;
    esac
    [ "$t" != "$BUILD_ROOT" ] || die "refusing to delete the build root itself"
    if [ -e "$t" ]; then log "removing $t"; rm -rf -- "$t"; fi
}

BUILD_ROOT="$(realpath -m -- "$BUILD_ROOT")"
case "$BUILD_ROOT" in
    /|"$HOME"|"") die "unsafe build root: '$BUILD_ROOT'" ;;
esac

if [ -d "$BUILD_ROOT" ]; then
    [ -e "$BUILD_ROOT/.difiw-build-root" ] || die "$BUILD_ROOT has no .difiw-build-root marker; not touching it"
    safe_rm "$BR_OUT"
    safe_rm "$MIRROR"
    safe_rm "$BUILD_ROOT/build.log"
    for a in "$@"; do
        if [ "$a" = "--all" ]; then
            safe_rm "$BR_DL"
            safe_rm "$BR_DIR"
        fi
    done
else
    log "no build tree at $BUILD_ROOT"
fi

for a in "$@"; do
    if [ "$a" = "--artifacts" ]; then
        log "removing artifacts in $ARTIFACTS"
        rm -f -- "$ARTIFACTS"/difiw-os-*.iso "$ARTIFACTS"/SHA256SUMS "$ARTIFACTS"/*.txt "$ARTIFACTS"/*.log
    fi
done
log "clean done"
