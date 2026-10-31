#!/bin/bash
# Downloads the official CEF minimal binary distribution (Spotify CDN) and verifies SHA-1.
set -euo pipefail
V="156.0.14+g4fa4ab5+chromium-156.0.8078.48"
SHA1="d091c82a8655113a34616c08cb340bf92310298d"
D="$HOME/netline-cef"; mkdir -p "$D"; cd "$D"
F="cef_binary_${V}_linux64_minimal.tar.bz2"
[ -s "$F" ] || curl -fL -o "$F" "https://cef-builds.spotifycdn.com/$(printf '%s' "$F" | sed 's/+/%2B/g')"
echo "$SHA1  $F" | sha1sum -c -
[ -d cef ] || { mkdir cef && tar -xjf "$F" -C cef --strip-components=1; }
ls cef
