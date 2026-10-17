#!/bin/bash
# Boot -> gameplay, then 12s `sample` + GPU utilization. Usage: profile.sh <tag> [run.sh args]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="$1"; shift
P="$ROOT/private/prof"
(timeout 230 "$ROOT/scripts/run.sh" "$@" > "$P/profile-$TAG.txt" 2>&1 &)
"$ROOT/scripts/to-gameplay.sh"
sleep 8
PID=$(pgrep -x mgr)
(for _ in $(seq 12); do ioreg -r -d 1 -c IOAccelerator -w0 | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2; sleep 1; done > "$P/gpu-util-$TAG.txt") &
sample "$PID" 12 -file "$P/sample-$TAG.txt" >/dev/null 2>&1
wait
pkill -x mgr; sleep 2
echo "GPU util: $(tr '\n' ' ' < "$P/gpu-util-$TAG.txt")"
python3 -I "$ROOT/tools/analysis/fps_summary.py" "$P/profile-$TAG.txt"
python3 -I "$ROOT/tools/analysis/thread_busy.py" "$P/sample-$TAG.txt"
