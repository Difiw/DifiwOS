#!/bin/bash
# Memory (PSS, all Netline processes summed) and idle CPU of Netline, 5 repeats each, medians printed.
# Usage (inside Linux/WSL, ideally on an idle machine): tests/netline/measure.sh [build-dir]
BIN="${1:-$HOME/nl-src-all/netline-cef/build}"
cd "$BIN" || exit 2
pids() { pgrep -f "$BIN/netline" ; }
pss_kb() { local t=0 p; for p in $(pids); do t=$((t + $(awk '/^Pss:/ {s+=$2} END {print s+0}' /proc/$p/smaps_rollup 2>/dev/null))); done; echo $t; }
cpu_ticks() { local t=0 p; for p in $(pids); do t=$((t + $(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null || echo 0))); done; echo $t; }
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
measure() {  # label, args...
  local label="$1"; shift
  local mem=() cpu=()
  for i in 1 2 3 4 5; do
    ./netline --private "$@" >/dev/null 2>&1 &
    sleep 20
    mem+=("$(pss_kb)")
    c1=$(cpu_ticks); sleep 10; c2=$(cpu_ticks)
    cpu+=("$(awk -v a=$c1 -v b=$c2 -v n=$(nproc) 'BEGIN {printf "%.2f", (b-a)/100/10/n*100}')")
    pkill -f "$BIN/netline"; sleep 3
  done
  echo "$label: PSS(MB) runs=[$(printf '%s ' "${mem[@]}" | awk '{for(i=1;i<=NF;i++) printf "%d ", $i/1024}')] median=$(printf '%s\n' "${mem[@]}" | median | awk '{print int($1/1024)}')   idleCPU(% of all cores) runs=[${cpu[*]}] median=$(printf '%s\n' "${cpu[@]}" | median)"
}
echo "host: $(nproc) CPUs, $(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MB RAM, $(uname -r)"
measure "empty start page"
measure "ya.ru (one tab)" https://ya.ru/
