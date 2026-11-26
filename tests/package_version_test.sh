#!/bin/bash
# package-macos.sh must refuse a malformed MGR_VERSION before building anything.
set -uo pipefail
SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/scripts/package-macos.sh"
failed=0
for bad in "1.2" "v1.2.3" "1.2.3-beta" "1.02.3" "abc"; do
  # MGR_BUILD_JOBS=0 stops an accepted version right after validation.
  out="$(MGR_VERSION="$bad" MGR_BUILD_JOBS=0 "$SCRIPT" 2>&1)"
  [[ "$out" == *"MGR_VERSION must be"* ]] || { echo "FAIL: accepted '$bad': $out"; failed=1; }
done
for good in "0.1.0" "1.10.3"; do
  out="$(MGR_VERSION="$good" MGR_BUILD_JOBS=0 "$SCRIPT" 2>&1)"
  [[ "$out" == *"MGR_BUILD_JOBS must be"* ]] || { echo "FAIL: rejected '$good': $out"; failed=1; }
done
[[ $failed == 0 ]] && echo "package version checks passed"
exit $failed
