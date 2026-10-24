#!/bin/bash
# Count calls per guest frame for every XDK D3D function on a frame-keyed route
# (title, then gameplay), focus-free. Needs a build configured with
# -DMGR_D3D_CENSUS=ON. Writes private/d3d/census-<tag>.<frame>.tsv, one file
# per 300-frame window, plus title and gameplay captures.
# Usage: scripts/d3d-census.sh <tag> [cvars...]
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="$1"; shift
OUT="$ROOT/private/d3d"
USER_DIR="$ROOT/private/user-native"
RUN_TIMEOUT=420
mkdir -p "$OUT"
[ -d "$USER_DIR" ] || cp -R "$ROOT/private/user" "$USER_DIR"
cat > "$OUT/census-$TAG-script.txt" <<SCRIPT
1500 capture census-$TAG-title
3000 press START 6
3300+300*4 press A 6
4600 stick 0 32767 2000
5400 capture census-$TAG-walk
6610 quit
SCRIPT
cp "${MGR_TEST_CONFIG:-$OUT/native-test.toml}" "$OUT/census-$TAG-mgr.toml"
MGR_USER_DATA_ROOT="$USER_DIR" MGR_CONFIG_PATH="$OUT/census-$TAG-mgr.toml" \
  SDL_MAC_BACKGROUND_APP=1 MGR_TEST_HIDDEN=1 MGR_TEST_SCRIPT="$OUT/census-$TAG-script.txt" \
  MGR_CAPTURE_DIR="$OUT" MGR_D3D_CENSUS=1 MGR_D3D_CENSUS_OUT="$OUT/census-$TAG" \
  timeout "$RUN_TIMEOUT" "$ROOT/scripts/run.sh" "$@" > "$OUT/census-$TAG.txt" 2>&1
echo "exit: $? (log $OUT/census-$TAG.txt)"
