#!/bin/bash
# Builds the SDL development variant inside WSL: build-dev.sh  (output: ~/nl-src/build)
set -e
rsync -a --delete --exclude build /mnt/w/ProjectCode/DifiwOS/src/ ~/nl-src-all/
mkdir -p ~/nl-src-all/netline-cef && cd ~/nl-src-all/netline-cef
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNETLINE_BACKEND=sdl >/dev/null
cmake --build build 2>&1 | grep -E "error|FAILED|warning" || true
ls -la build/netline
