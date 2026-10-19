#!/bin/bash
# Prepare the SDK source in this worktree; no resets, shared writable trees,
# generated-code changes or modifications to the reference checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/vendor/rexglue-sdk"
BASE=bd833a2ab69d305f99ddf57418fc38ffec8af93d
SOURCE="${1:-https://github.com/rexglue/rexglue-sdk.git}"
if [ ! -e "$SDK" ]; then
    mkdir -p "$ROOT/vendor"
    git clone --no-hardlinks --no-checkout "$SOURCE" "$SDK"
    git -C "$SDK" checkout --detach "$BASE"
fi
if [ "$(git -C "$SDK" rev-parse HEAD)" != "$BASE" ]; then
    echo "SDK base differs from $BASE; preserve it and prepare a separate checkout." >&2
    exit 1
fi
for patch in xenos-gpu-mgr.patch xenos-metal4-shared-translator.patch xenos-metal4-shared-edram.patch xenos-metal4-sample-mask-phi.patch; do
    if git -C "$SDK" apply --reverse --check "$ROOT/patches/$patch" 2>/dev/null; then
        echo "$patch already applied"
    else
        git -C "$SDK" apply --check "$ROOT/patches/$patch"
        git -C "$SDK" apply "$ROOT/patches/$patch"
    fi
done
GLSLANG="$ROOT/vendor/glslang"
GLSLANG_BASE=f4f1d8a352ca1908943aea2ad8c54b39b4879080
if [ ! -e "$GLSLANG" ]; then
    git init "$GLSLANG"
    git -C "$GLSLANG" remote add origin https://github.com/KhronosGroup/glslang.git
    git -C "$GLSLANG" fetch --depth 1 origin "$GLSLANG_BASE"
    git -C "$GLSLANG" checkout --detach FETCH_HEAD
fi
if [ "$(git -C "$GLSLANG" rev-parse HEAD)" != "$GLSLANG_BASE" ]; then
    echo "glslang revision differs from the pinned SDK headers; preserve it and prepare a separate checkout." >&2
    exit 1
fi
echo "SDK sources prepared; supply the existing binary SDK prefix when configuring."
