#!/bin/bash
# Run recompiled MGR. Usage: scripts/run.sh [extra cvars...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/private/logs" "$ROOT/private/user" "$ROOT/private/cache"
N=$(ls "$ROOT"/private/logs/run-*.log 2>/dev/null | wc -l | tr -d ' ')
LOG="$ROOT/private/logs/run-$(printf %02d $((N+1))).log"
echo "log: $LOG"
cd "$ROOT/build"
./mgr \
  --gpu_plugin=xenos \
  --game_data_root="$ROOT/private/game" \
  --user_data_root="$ROOT/private/user" \
  --cache_root="$ROOT/private/cache" \
  --mnk_mode=true \
  --log_file="$LOG" \
  "$@"
echo "exit: $?" | tee -a "$LOG"
