#!/bin/bash
# A/B a post-processing experiment on a frame-keyed route: title animation,
# then gameplay walking forward (the window postfx_summary.py reads). Runs are
# focus-free: hidden window, background app, scripted pad input
# (src/mgr_test_script.cpp). Two runs per tag:
#   level 1  MGR_GPU_TIMING=1: guest fps and GPU busy per frame (unperturbed)
#   level 2  MGR_GPU_TIMING=2: per pass / per guest shader GPU and CP time,
#            resolve and texture-load counts, OT render-callback CPU time.
# Usage: scripts/bench-postfx.sh <tag> [cvars...]
#   scripts/bench-postfx.sh base
#   scripts/bench-postfx.sh nobloom --mgr_bloom=false
#   python3 -I tools/analysis/postfx_summary.py base nobloom
# Saves come from a copy (private/user-postfx) and each run gets its own copy
# of build/mgr.toml (or MGR_BENCH_CONFIG), so the owner's files stay untouched.
# Captures of the walk land next to the logs as <tag>-l<level>-walk-*.ppm.
# Do not run while another mgr instance runs.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="$1"; shift
PROF="$ROOT/private/prof"
NAMES="$PROF/mgr_shader_names.tsv"
USER_DIR="$ROOT/private/user-postfx"
RUN_TIMEOUT=420
mkdir -p "$PROF"
[ -d "$USER_DIR" ] || cp -R "$ROOT/private/user" "$USER_DIR"
if [ ! -f "$NAMES" ]; then
  python3 -I "$ROOT/tools/analysis/shader_names.py" \
    "$ROOT/private/cache/shaders/shareable/4B4E080A.xsh" "$ROOT/private/shaders" "$NAMES"
fi
for LEVEL in 1 2; do
  OUT="$PROF/postfx-$TAG-l$LEVEL"
  echo "== $TAG level $LEVEL -> $OUT.txt"
  cp "${MGR_BENCH_CONFIG:-$ROOT/build/mgr.toml}" "$OUT-mgr.toml"
  cat > "$OUT-script.txt" <<EOF
1800 mark title-window
3000 press START 6
3300+300*4 press A 6
4600 mark gameplay-window
4600 stick 0 32767 2000
5400 capture $TAG-l$LEVEL-walk
6600 capture $TAG-l$LEVEL-walk-end
6610 quit
EOF
  MGR_USER_DATA_ROOT="$USER_DIR" MGR_CONFIG_PATH="$OUT-mgr.toml" \
    SDL_MAC_BACKGROUND_APP=1 MGR_TEST_HIDDEN=1 MGR_TEST_SCRIPT="$OUT-script.txt" \
    MGR_CAPTURE_DIR="$PROF" MGR_GPU_TIMING=$LEVEL MGR_SHADER_NAMES="$NAMES" \
    MGR_OT_PROFILE=$((LEVEL == 2)) MGR_FRAME_CSV="$OUT-frame.csv" \
    MGR_GPU_TIMING_CSV="$OUT-gpu.csv" \
    timeout "$RUN_TIMEOUT" "$ROOT/scripts/run.sh" --mgr_renderer=xenos --mgr_output_resolution=source "$@" \
    > "$OUT.txt" 2>&1
  echo "   exit $?"
done
python3 -I "$ROOT/tools/analysis/fps_summary.py" "$PROF/postfx-$TAG-l1.txt"
