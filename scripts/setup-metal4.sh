#!/bin/bash
# Immutable, independent source checkouts. Never modify the reference workspace.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/vendor"
checkout() {
  local name="$1" url="$2" revision="$3"
  local destination="$ROOT/vendor/$name"
  if [ ! -d "$destination" ]; then
    git init "$destination"
    git -C "$destination" remote add origin "$url"
    git -C "$destination" fetch --depth 1 origin "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
  fi
  if [ "$(git -C "$destination" rev-parse HEAD)" != "$revision" ]; then
    echo "Unexpected $name revision; preserve that checkout and supply a separate dependency path." >&2
    exit 1
  fi
  if [ -n "$(git -C "$destination" status --porcelain --untracked-files=no)" ]; then
    echo "Modified $name checkout; refusing to overwrite it." >&2
    exit 1
  fi
}
checkout metal-cpp https://github.com/apple/metal-cpp.git 27c4382b7151d55a51692cdcb27aaa98752240de
checkout spirv-cross https://github.com/KhronosGroup/SPIRV-Cross.git 300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1
echo "Metal 4 dependencies ready. Configure with MGR_BUILD_GAME=OFF, MGR_BUILD_METAL4=ON and your SDL3 prefix."
