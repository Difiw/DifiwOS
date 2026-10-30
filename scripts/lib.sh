#!/usr/bin/env bash
# Shared settings for Difiw OS scripts. Source it; do not execute it.
# SPDX-License-Identifier: MIT
# shellcheck disable=SC2034

set -euo pipefail

# Project root, derived from this file's location (never a hardcoded path).
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../versions.env
. "$PROJECT_ROOT/versions.env"
DIFIW_VERSION="$(tr -d '[:space:]' < "$PROJECT_ROOT/VERSION")"

# Build tree lives on the Linux filesystem (fast, correct permissions/symlinks).
BUILD_ROOT="${DIFIW_BUILD_ROOT:-$HOME/difiw-build}"
BR_DIR="$BUILD_ROOT/buildroot-$BUILDROOT_VERSION"
BR_OUT="$BUILD_ROOT/output"
BR_DL="$BUILD_ROOT/dl"
MIRROR="$BUILD_ROOT/project"
BUILD_MARKER="$BUILD_ROOT/.difiw-build-root"
ARTIFACTS="$PROJECT_ROOT/artifacts"
ISO_NAME="difiw-os-${DIFIW_VERSION}-x86_64.iso"

log()  { printf '\033[1;34m[difiw]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[difiw] warning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[difiw] error:\033[0m %s\n' "$*" >&2; exit 1; }

require_linux() {
    [ "$(uname -s)" = "Linux" ] || die "run this script inside Linux/WSL (Buildroot needs Linux). From Windows: powershell scripts/wsl.ps1 <target>"
}

init_build_root() {
    mkdir -p "$BUILD_ROOT" "$BR_DL"
    [ -e "$BUILD_MARKER" ] || date -u +%FT%TZ > "$BUILD_MARKER"
}

jobs_count() {
    local cpus mem_mb by_mem
    cpus="$(nproc)"
    mem_mb="$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo)"
    by_mem=$(( mem_mb / 1500 ))
    [ "$by_mem" -ge 1 ] || by_mem=1
    [ "$cpus" -le "$by_mem" ] && echo "$cpus" || echo "$by_mem"
}
