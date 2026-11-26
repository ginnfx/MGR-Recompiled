#!/bin/bash
# Download the pinned Sparkle release into tools/sdk/sparkle/ (gitignored).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=2.10.1
SHA256=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
DIR="$ROOT/tools/sdk/sparkle"
ARCHIVE="$DIR/Sparkle-$VERSION.tar.xz"
mkdir -p "$DIR"
[[ -f "$ARCHIVE" ]] || curl -fsSL -o "$ARCHIVE" \
  "https://github.com/sparkle-project/Sparkle/releases/download/$VERSION/Sparkle-$VERSION.tar.xz"
echo "$SHA256  $ARCHIVE" | shasum -a 256 -c - >/dev/null ||
  { echo "fetch-sparkle: $ARCHIVE does not match the pinned hash" >&2; exit 1; }
rm -rf "${DIR:?}/$VERSION"
mkdir -p "$DIR/$VERSION"
tar -xJf "$ARCHIVE" -C "$DIR/$VERSION"
echo "Sparkle $VERSION in $DIR/$VERSION"
