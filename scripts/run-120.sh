#!/bin/bash
# Experimental fractional frame scaling; run.sh defaults remain unchanged.
# MGR_FRAME_SCALE_FORCE_ELAPSED=1 also overrides the guest's forced-1 mode.
# MGR_FRAME_INTERVAL_ONE=0 retains the guest's requested retrace interval.
# MGR_GPU_WAIT_BACKOFF=0 retains the original guest polling behavior.
# MGR_TIMING_PACE=0 disables the host 120-Hz cap for isolation.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export MGR_USER_DATA_ROOT="${MGR_USER_DATA_ROOT:-$ROOT/private/user-timing-120}"
if [ ! -d "$MGR_USER_DATA_ROOT" ]; then
  mkdir -p "$(dirname "$MGR_USER_DATA_ROOT")"
  if [ -d "$ROOT/private/user" ]; then
    cp -R "$ROOT/private/user" "$MGR_USER_DATA_ROOT"
  else
    mkdir -p "$MGR_USER_DATA_ROOT"
  fi
fi
echo "experimental saves: $MGR_USER_DATA_ROOT"
export MGR_FPS=0
exec "$(dirname "$0")/run.sh" \
  --mgr_frame_rate=120 \
  --vsync=true --video_mode_refresh_rate=120 \
  --vulkan_allow_present_mode_immediate=true "$@"
