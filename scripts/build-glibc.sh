#!/usr/bin/env bash
# Full 0.5.6 build in its own tree (~/difiw-build-glibc) so the old musl tree stays as a rollback.
export DIFIW_BUILD_ROOT="${DIFIW_BUILD_ROOT:-$HOME/difiw-build-glibc}"
exec "$(dirname "${BASH_SOURCE[0]}")/build.sh"
