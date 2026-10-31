#!/bin/sh
# Starts Netline (Chromium/CEF) inside a Difiw desktop window. HOME is the per-app home, TMPDIR the per-app tmp.
export LD_LIBRARY_PATH=/opt/netline
export CHROME_DEVEL_SANDBOX=/opt/netline/chrome-sandbox
mkdir -p "$HOME/.netline" "$HOME/Downloads" 2>/dev/null
cd /opt/netline || exit 1
exec /opt/netline/netline "$@"
