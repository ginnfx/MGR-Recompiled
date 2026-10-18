#!/bin/bash
# Merge profiles from manual gameplay. Does not run the game or build anything.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/private/pgo/mgr.profdata}"
if [ "$#" -gt 0 ]; then shift; fi
if [ "$#" -eq 0 ]; then
  set -- "$ROOT/build/pgo-raw" "$ROOT/build-tools/rexruntime/pgo-raw"
fi
RAW=()
for DIR in "$@"; do
  if [ ! -d "$DIR" ]; then
    echo "Profile directory does not exist: $DIR" >&2
    exit 2
  fi
  while IFS= read -r -d '' FILE; do RAW+=("$FILE"); done < <(find "$DIR" -type f -name '*.profraw' -print0)
done
if [ "${#RAW[@]}" -eq 0 ]; then
  echo "No .profraw files found. A GENERATE build needs manual gameplay and normal exit first." >&2
  exit 2
fi
mkdir -p "$(dirname "$OUT")"
xcrun llvm-profdata merge -o "$OUT" "${RAW[@]}"
echo "Merged ${#RAW[@]} raw profile files into $OUT"
