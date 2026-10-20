#!/bin/bash
# Launch the local app through LaunchServices, with isolated experiment state.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL="${1:?label required}"; shift
MODE="${1:-native}"; if [ "$#" -gt 0 ]; then shift; fi
case "$MODE" in
  original|sample-original) export MGR_NATIVE_MATH=0 MGR_NATIVE_MEMORY=0 MGR_NATIVE_MATH_VERIFY=0 MTL_DEBUG_LAYER=0 ;;
  native|sample-native) export MGR_NATIVE_MATH=1 MGR_NATIVE_MEMORY=1 MGR_NATIVE_MATH_VERIFY=0 MTL_DEBUG_LAYER=0 ;;
  verify) export MGR_NATIVE_MATH=1 MGR_NATIVE_MEMORY=1 MGR_NATIVE_MATH_VERIFY=1 MTL_DEBUG_LAYER=1 ;;
  *) echo "mode must be original, native or verify" >&2; exit 2 ;;
esac
case "$MODE" in sample-*) export MGR_METAL_HUD=1 ;; esac
OUT="$ROOT/private/native-rewrite/$LABEL"
APP="${MGR_APP_PATH:-$ROOT/private/native-rewrite/MGR Native Rewrite.app}"
mkdir -p "$OUT/user" "$OUT/cache"
if [ ! -f "$OUT/.seeded" ]; then
  cp -R "$ROOT/private/user/." "$OUT/user/"
  if [ -d "$ROOT/private/baseline/warmed-cache" ]; then
    cp -R "$ROOT/private/baseline/warmed-cache/." "$OUT/cache/"
  fi
  touch "$OUT/.seeded"
fi
exec open -n -W -a "$APP" --stdout "$OUT/stdout.log" --stderr "$OUT/stderr.log" \
  --env "MGR_FRAME_CSV=$OUT/frames.csv" \
  --env "MGR_NATIVE_SHADERS=$ROOT/private/mgr_native_shaders.xrnb" \
  --env "MGR_NATIVE_MATH=${MGR_NATIVE_MATH:-1}" \
  --env "MGR_NATIVE_MATH_VERIFY=${MGR_NATIVE_MATH_VERIFY:-0}" \
  --env "MGR_NATIVE_MEMORY=${MGR_NATIVE_MEMORY:-1}" \
  --env "MGR_D3D_TRACE=${MGR_D3D_TRACE:-0}" \
  --env "MGR_GPU_STATS=${MGR_GPU_STATS:-1}" \
  --env "MTL_HUD_ENABLED=${MGR_METAL_HUD:-0}" \
  --env "MTL_HUD_LOG_ENABLED=${MGR_METAL_HUD:-0}" \
  --env "MTL_DEBUG_LAYER=${MTL_DEBUG_LAYER:-0}" \
  --args --gpu_plugin=xenos --game_data_root="$ROOT/private/game" \
  --user_data_root="$OUT/user" --cache_root="$OUT/cache" \
  --mnk_mode=true --vulkan_nonblocking_guest_present=true \
  --mgr_skip_intro_logos=true --log_file="$OUT/game.log" "$@"
