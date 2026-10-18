#!/bin/bash
# Build/stage all source components with the same compiler policy. Never launches
# the game. Extra arguments are shared CMake cache options, e.g. -DMGR_PGO_MODE=USE.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${MGR_BUILD_JOBS:-4}"
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "MGR_BUILD_JOBS must be a positive integer" >&2
  exit 2
fi
cmake -S "$ROOT/runtime" -B "$ROOT/build-tools/rexruntime" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
  -DMGR_PGO_MODE=OFF "$@"
cmake --build "$ROOT/build-tools/rexruntime" --target rexruntime --parallel "$JOBS"
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$ROOT/tools/sdk/mac-arm64" \
  -DMGR_PGO_MODE=OFF "$@"
cmake --build "$ROOT/build" --target mgr --parallel "$JOBS"
# The runtime can change without a game relink; POST_BUILD alone misses that case.
cmake -E copy_if_different "$ROOT/build-tools/rexruntime/librexruntimerd.dylib" "$ROOT/build/"
