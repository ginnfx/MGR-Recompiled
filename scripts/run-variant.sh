#!/bin/bash
# A/B the runtime without rebuilding. Usage: scripts/run-variant.sh <full|no501|sdk> [run.sh args]
#   full  = source-built runtime with all fixes (default staged build/librexruntimerd.dylib)
#   no501 = source-built runtime without PR #501 (WaitMultiple epoch sleep)
#   sdk   = original prebuilt SDK runtime (no #507/#501/#497)
# run.sh sets DYLD_LIBRARY_PATH (searched before @rpath) from MGR_RUNTIME_DIR; each variant
# dir holds only the runtime dylib, so the in-tree GPU plugin is still used.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:-full}"; shift || true
case "$VARIANT" in
  full) exec "$ROOT/scripts/run.sh" "$@" ;;
  no501|sdk) DIR="$ROOT/build-tools/rt-variants/$VARIANT" ;;
  *) echo "unknown variant: $VARIANT (full|no501|sdk)" >&2; exit 2 ;;
esac
[ -f "$DIR/librexruntimerd.dylib" ] || { echo "missing $DIR/librexruntimerd.dylib" >&2; exit 1; }
echo "runtime variant: $VARIANT ($DIR)"
MGR_RUNTIME_DIR="$DIR" exec "$ROOT/scripts/run.sh" "$@"
