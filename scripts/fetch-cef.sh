#!/usr/bin/env bash
# Downloads the official CEF minimal binary distribution for Linux x64 and verifies its SHA-1 (published in the CEF index).
# The file goes to $BR_DL/difiw-netline/ where the Buildroot package difiw-netline expects it.
# SPDX-License-Identifier: MIT
set -euo pipefail
# shellcheck source=lib.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
CEF_VERSION="156.0.14+g4fa4ab5+chromium-156.0.8078.48"
CEF_SHA1="d091c82a8655113a34616c08cb340bf92310298d"
NAME="cef_binary_${CEF_VERSION}_linux64_minimal.tar.bz2"
DEST="$BR_DL/difiw-netline/cef_binary_linux64_minimal.tar.bz2"
mkdir -p "$(dirname "$DEST")"
if [ ! -s "$DEST" ] || ! echo "$CEF_SHA1  $DEST" | sha1sum -c --quiet - 2>/dev/null; then
    log "downloading CEF $CEF_VERSION (about 330 MB)"
    curl -fL --retry 3 -o "$DEST" "https://cef-builds.spotifycdn.com/$(printf '%s' "$NAME" | sed 's/+/%2B/g')"
    echo "$CEF_SHA1  $DEST" | sha1sum -c -
fi
log "CEF ready: $DEST"
