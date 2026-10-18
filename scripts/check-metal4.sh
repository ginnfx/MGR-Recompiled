#!/bin/bash
# Functional GPU checks, including a real archive reload. Not a benchmark.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/metal4}"
SMOKE="$BUILD/gpu/metal/mgr_metal4_smoke"
CHECK="$(mktemp -d "$BUILD/check.XXXXXX")"
cmake --build "$BUILD" --target mgr_metal4_smoke -j2
# Keep instrumentation out of both normal runs, even in a diagnostic shell.
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_ENABLE_PIPELINES \
    -u MTL_SHADER_VALIDATION_DISABLE_PIPELINES "$SMOKE" --cache "$CHECK/cache" > "$CHECK/cold.log" 2>&1
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_ENABLE_PIPELINES \
    -u MTL_SHADER_VALIDATION_DISABLE_PIPELINES "$SMOKE" --cache "$CHECK/cache" > "$CHECK/warm.log" 2>&1
rg -q 'archive_loaded=0' "$CHECK/cold.log"
rg -q 'archive_loaded=1' "$CHECK/warm.log"
# This is a disposable test cache. A truncated archive must recover through
# source compilation, without damaging or borrowing the Vulkan cache.
ARCHIVE="$(rg --files "$CHECK/cache" -g 'pipelines.metallib')"
test -f "$ARCHIVE"
printf 'invalid test archive' > "$ARCHIVE"
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_ENABLE_PIPELINES \
    -u MTL_SHADER_VALIDATION_DISABLE_PIPELINES "$SMOKE" --cache "$CHECK/cache" > "$CHECK/recovery.log" 2>&1
rg -q 'archive_loaded=0' "$CHECK/recovery.log"
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
    "$SMOKE" --cache "$CHECK/cache" --capture "$CHECK/frame.ppm" > "$CHECK/validation.log" 2>&1
if rg -n 'FAIL|Validation Error|Shader Validation Error|archive rejected|cache write failed' "$CHECK"/*.log; then
    echo "Metal check failed; inspect $CHECK" >&2
    exit 1
fi
cat "$CHECK/cold.log" "$CHECK/warm.log" "$CHECK/recovery.log" "$CHECK/validation.log"
echo "Evidence: $CHECK"
