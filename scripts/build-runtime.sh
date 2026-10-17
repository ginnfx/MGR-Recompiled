#!/bin/bash
# Build the ReXGlue runtime from vendor/rexglue-sdk with MGR fixes
# (runtime/CMakeLists.txt) and stage it next to build/mgr. Usage: scripts/build-runtime.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build-tools/rexruntime"
git -C "$ROOT/vendor/rexglue-sdk" submodule update --init --depth 1 \
  thirdparty/cli11 thirdparty/imgui thirdparty/FFmpeg thirdparty/libmspack
if [ ! -f "$OUT/build.ninja" ]; then
  # Apple clang explicitly: the swiftly clang shim on PATH is broken.
  cmake -S "$ROOT/runtime" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++
fi
ninja -C "$OUT" -j4 rexruntime  # -j4: stays within 8 GB
cp "$OUT/librexruntimerd.dylib" "$ROOT/build/"
echo "staged $OUT/librexruntimerd.dylib -> build/"
