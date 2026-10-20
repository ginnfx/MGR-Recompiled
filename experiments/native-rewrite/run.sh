#!/bin/bash
# Independent experiment launcher: run.sh <label> [game cvars...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL="${1:?label required}"; shift
OUT="$ROOT/private/native-rewrite/$LABEL"
mkdir -p "$OUT/user" "$OUT/cache"
# Seed saves explicitly in each experiment; never write through the game-data link.
if [ ! -f "$OUT/.seeded" ]; then
  cp -R "$ROOT/private/user/." "$OUT/user/"
  touch "$OUT/.seeded"
fi
export MGR_FRAME_CSV="$OUT/frames.csv"
export MGR_NATIVE_SHADERS="$ROOT/private/mgr_native_shaders.xrnb"
export MGR_GPU_STATS=1
cd "${MGR_BUILD_DIR:-$ROOT/build}"
exec ./mgr --gpu_plugin=xenos --game_data_root="$ROOT/private/game" \
  --user_data_root="$OUT/user" --cache_root="$OUT/cache" \
  --mnk_mode=true --vulkan_nonblocking_guest_present=true \
  --mgr_skip_intro_logos=true --log_file="$OUT/game.log" "$@"
