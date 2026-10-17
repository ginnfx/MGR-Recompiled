#!/bin/bash
# Boot -> title -> VR tutorial; report fps. Usage: bench.sh <tag> [run.sh args...]
# Env vars (e.g. MVK_CONFIG_*) pass through to the game.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="$1"; shift
OUT="$ROOT/private/prof/bench-$TAG.txt"
mkdir -p "$ROOT/private/prof"
(timeout 260 "$ROOT/scripts/run.sh" "$@" > "$OUT" 2>&1 &)
"$ROOT/scripts/to-gameplay.sh"
sleep 40
pkill -x mgr; sleep 2
python3 -I "$ROOT/tools/analysis/fps_summary.py" "$OUT"
