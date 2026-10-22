#!/bin/bash
# Run recompiled MGR. Usage: scripts/run.sh [extra cvars...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/private/logs" "$ROOT/private/user" "$ROOT/private/cache"
N=$(ls "$ROOT"/private/logs/run-*.log 2>/dev/null | wc -l | tr -d ' ')
LOG="$ROOT/private/logs/run-$(printf %02d $((N+1))).log"
echo "log: $LOG"
cd "$ROOT/build"
# Native (offline XenosRecomp) pixel shaders; MGR_NATIVE_PS=0 forces Xenia shaders.
export MGR_NATIVE_SHADERS="${MGR_NATIVE_SHADERS:-$ROOT/private/mgr_native_shaders.xrnb}"
# MGR_RUNTIME_DIR (scripts/run-variant.sh): load librexruntimerd.dylib from there.
# Exported here rather than inherited: SIP strips DYLD_* when /bin/bash starts.
if [ -n "${MGR_RUNTIME_DIR:-}" ]; then
  export DYLD_LIBRARY_PATH="$MGR_RUNTIME_DIR"
  echo "runtime override: $MGR_RUNTIME_DIR"
fi
./mgr \
  --gpu_plugin=xenos \
  --game_data_root="$ROOT/private/game" \
  --user_data_root="${MGR_USER_DATA_ROOT:-$ROOT/private/user}" \
  --cache_root="$ROOT/private/cache" \
  --vulkan_nonblocking_guest_present=true \
  --log_file="$LOG" \
  "$@"
echo "exit: $?" | tee -a "$LOG"
