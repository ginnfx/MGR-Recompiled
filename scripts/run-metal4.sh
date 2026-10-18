#!/bin/bash
# Experimental native plugin. Guest draws/resolves currently stop with exit 2.
# Usage: MGR_GAME_DATA_ROOT=/path/to/game scripts/run-metal4.sh [extra cvars...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${MGR_METAL4_BUILD_DIR:-$ROOT/build/metal4-game}"
if [ ! -x "$BUILD/mgr" ]; then
    echo "Build mgr with MGR_BUILD_METAL4=ON and MGR_METAL4_XENOS_INTEGRATION=ON first." >&2
    exit 1
fi
mkdir -p "$ROOT/private/logs" "$ROOT/private/metal4-user" "$ROOT/private/metal4-cache"
cd "$BUILD"
exec ./mgr \
    --gpu_plugin=xenos-metal4 \
    --game_data_root="${MGR_GAME_DATA_ROOT:-$ROOT/private/game}" \
    --user_data_root="${MGR_USER_DATA_ROOT:-$ROOT/private/metal4-user}" \
    --cache_root="$ROOT/private/metal4-cache" \
    --log_file="$ROOT/private/logs/metal4-$(date +%Y%m%d-%H%M%S).log" \
    --mnk_mode=true "$@"
