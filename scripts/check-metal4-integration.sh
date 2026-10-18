#!/bin/bash
# Runtime plugin/frontend checks. Add --window to exercise the SDK SDL surface.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${MGR_METAL4_BUILD_DIR:-$ROOT/build/metal4}"
cmake --build "$BUILD" --target mgr_metal4_integration_checks -j2
CHECK="$(mktemp -d "$BUILD/integration.XXXXXX")"
if [ -n "${MGR_XENOS_SHADER_CACHE:-}" ]; then
    set -- "$@" --shader-cache "$MGR_XENOS_SHADER_CACHE"
fi
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
    "$BUILD/mgr_metal4_integration_checks" "$@" > "$CHECK/validation.log" 2>&1
status=0
"$BUILD/mgr_metal4_integration_checks" --reject-draw > "$CHECK/reject.log" 2>&1 || status=$?
test "$status" = 2
rg -q 'unsupported guest operation: Xenos draw' "$CHECK/reject.log"
rg -q 'PASS native plugin ABI' "$CHECK/validation.log"
if rg -n 'FAIL|Validation Error|Shader Validation Error|implemented in both|already registered' "$CHECK/validation.log"; then
    echo "Native integration check failed; inspect $CHECK" >&2
    exit 1
fi
cat "$CHECK/validation.log"
echo "PASS unsupported guest draws fail explicitly (exit 2)"
echo "Evidence: $CHECK"
