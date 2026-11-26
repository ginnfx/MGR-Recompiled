#!/bin/bash
# Fuzz the disc executable and DLC package parsers with ASan and UBSan.
#   scripts/fuzz.sh [seconds per target, default 60]
# Needs Homebrew LLVM (Apple clang ships no libFuzzer).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECONDS_EACH="${1:-60}"
CXX="${FUZZ_CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
die() { echo "fuzz: $*" >&2; exit 1; }
[[ "$SECONDS_EACH" =~ ^[1-9][0-9]*$ ]] || die "seconds must be a positive integer"
[[ -x "$CXX" ]] || die "no compiler at $CXX (brew install llvm, or set FUZZ_CXX)"
XXHASH="$(dirname "$(find "$ROOT/tools/sdk/mac-arm64/include" "$ROOT/vendor/rexglue-sdk" \
  -name xxhash.h -print -quit)")"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
for target in xex dlc; do
  define=()
  [[ $target == xex ]] && define=(-DMGR_FUZZ_XEX)
  "$CXX" -std=c++23 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
    -isysroot "$(xcrun --show-sdk-path)" -DXXH_INLINE_ALL ${define[@]+"${define[@]}"} \
    -I"$ROOT/src" -I"$XXHASH" "$ROOT/tests/fuzz/fuzz_parsers.cpp" \
    "$ROOT/src/mgr_dlc_package.cpp" "$ROOT/src/mgr_game_install.cpp" -o "$OUT/$target"
  mkdir -p "$OUT/corpus-$target"
  echo "== fuzzing $target for ${SECONDS_EACH}s"
  "$OUT/$target" -max_total_time="$SECONDS_EACH" -max_len=65536 -print_final_stats=1 \
    -artifact_prefix="$ROOT/build/fuzz-crash-" "$OUT/corpus-$target" 2>&1 | tail -12
done
