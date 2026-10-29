#!/bin/bash
# Build a self-contained "MGR Recompiled.app" and a drag-to-Applications .dmg in
# dist/. Uses its own build-release/ tree (deployment target below, no host CPU
# tune), so build/ and scripts/run.sh are untouched. Never launches the game.
#
#   scripts/package-macos.sh                 # ad-hoc signed
#   SIGN_IDENTITY="Developer ID Application: ..." scripts/package-macos.sh
#   MGR_SETUP_BACKGROUND=art.jpg scripts/package-macos.sh   # setup screen art
#
# Game files are never packaged: the app installs them from the player's own
# disc image on first launch. Extra arguments are passed to the game's CMake.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${MGR_BUILD_JOBS:-4}"
MIN_MACOS="14.0"
APP_NAME="MGR Recompiled"
VERSION="${MGR_VERSION:-0.1.0}"
SIGN_IDENTITY="${SIGN_IDENTITY:--}"
BUILD="$ROOT/build-release"
DIST="$ROOT/dist"
APP="$DIST/$APP_NAME.app"
RUNTIME="$BUILD/rexruntime/librexruntimerd.dylib"
SDK_LIB="$ROOT/tools/sdk/mac-arm64/lib"

die() { echo "package-macos: $*" >&2; exit 1; }

if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then die "MGR_BUILD_JOBS must be a positive integer"; fi
[[ -d "$ROOT/generated" ]] || die "missing generated/; run: tools/sdk/mac-arm64/bin/rexglue codegen mgr_manifest.toml"
[[ -f "$ROOT/private/mgr_native_shaders.xrnb" ]] || die "missing private/mgr_native_shaders.xrnb (renderer shader cache)"
[[ -z "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]] ||
  echo "package-macos: warning: working tree has uncommitted changes" >&2

# --- Build -------------------------------------------------------------------
# Source paths in __FILE__ and debug info become repo-relative, so the shipped
# binaries don't carry this machine's home folder. The runtime gets its own
# copy for that reason (it already targets macOS 13.3, runtime/CMakeLists.txt).
[[ "$ROOT" != *" "* ]] || die "the repo path must not contain spaces"
NO_PATHS="-ffile-prefix-map=$ROOT/="
PATH_FLAGS=(-DCMAKE_C_FLAGS="$NO_PATHS" -DCMAKE_CXX_FLAGS="$NO_PATHS"
            -DCMAKE_OBJC_FLAGS="$NO_PATHS" -DCMAKE_OBJCXX_FLAGS="$NO_PATHS")
cmake -S "$ROOT/runtime" -B "$BUILD/rexruntime" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
  -DMGR_PGO_MODE=OFF "${PATH_FLAGS[@]}"
cmake --build "$BUILD/rexruntime" --target rexruntime --parallel "$JOBS"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$ROOT/tools/sdk/mac-arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" -DMGR_CPU_TUNE= -DMGR_PGO_MODE=OFF \
  -DMGR_RUNTIME_LIB="$RUNTIME" "${PATH_FLAGS[@]}" "$@"
cmake --build "$BUILD" --target mgr --parallel "$JOBS"
cmake -E copy_if_different "$RUNTIME" "$BUILD/"

# --- Assemble the bundle -----------------------------------------------------
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
# Code goes in MacOS/ (the GPU plugin loader looks beside the executable);
# data goes in Resources/, or codesign rejects the bundle.
for f in mgr librexruntimerd.dylib libTracyClientrd.dylib \
         librexgpu-xenosrd.dylib librexgpu-mgrnullrd.dylib; do
  [[ -f "$BUILD/$f" ]] || die "missing build output $f"
  cp "$BUILD/$f" "$APP/Contents/MacOS/"
done
cp -R "$BUILD/vulkan" "$APP/Contents/Resources/vulkan"
find "$APP/Contents/Resources/vulkan" -name .DS_Store -delete
cp "$BUILD/mgr_native_shaders.xrnb" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES" \
   "$APP/Contents/Resources/"
# Setup screen fonts and art. The art is the icon artwork unless one is given
# explicitly: a local private/setup_background.jpg never ships by accident.
cp -R "$BUILD/setup" "$APP/Contents/Resources/setup"
cp "${MGR_SETUP_BACKGROUND:-$ROOT/cmake/macos/AppIcon-source.jpg}" \
   "$APP/Contents/Resources/setup/background.jpg"
if [[ -f "$ROOT/cmake/macos/AppIcon.icns" ]]; then
  cp "$ROOT/cmake/macos/AppIcon.icns" "$APP/Contents/Resources/"
fi
BUILD_NUMBER="$(git -C "$ROOT" rev-list --count HEAD)"
sed -e "s/@APP_NAME@/$APP_NAME/g" -e "s/@VERSION@/$VERSION/g" \
    -e "s/@BUILD_NUMBER@/$BUILD_NUMBER/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" \
    "$ROOT/cmake/macos/Info.plist.in" > "$APP/Contents/Info.plist"
plutil -lint -s "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

MACHO=()
while IFS= read -r f; do MACHO+=("$f"); done < <(
  find "$APP/Contents" -type f \( -name mgr -o -name '*.dylib' \) | sort)

# --- Symbols -----------------------------------------------------------------
# Keep .dSYMs (for crash reports) out of the shipped app, then drop debug info.
SYMBOLS="$DIST/symbols-$VERSION"
rm -rf "$SYMBOLS" && mkdir -p "$SYMBOLS"
for f in "${MACHO[@]}"; do
  case "$f" in "$APP/Contents/Resources/"*) continue ;; esac  # prebuilt MoltenVK
  xcrun dsymutil "$f" -o "$SYMBOLS/$(basename "$f").dSYM" 2>/dev/null ||
    echo "package-macos: warning: no dSYM for $(basename "$f")" >&2
  xcrun strip -S "$f"
done

# --- Checks ------------------------------------------------------------------
version_le() { [[ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" == "$1" ]]; }
for f in "${MACHO[@]}"; do
  name="${f#"$APP"/}"
  minos="$(otool -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
  version_le "$minos" "$MIN_MACOS" || die "$name requires macOS $minos (> $MIN_MACOS)"
  if otool -l "$f" | grep -E '^ +(path|name) ' | grep -Eq '/Users/|/opt/|/usr/local/'; then
    die "$name has a machine-specific rpath or load path"
  fi
  if LC_ALL=C grep -aqF -e "$ROOT" -e "$HOME/" "$f"; then
    die "$name contains a path from this machine"
  fi
  otool -L "$f" | tail -n +2 | awk '{print $1}' |
    grep -Ev '^(@rpath|@loader_path|@executable_path|/System/|/usr/lib/)' &&
    die "$name links a library outside the bundle and system"
done

# --- Sign --------------------------------------------------------------------
# Ad-hoc by default (what other recomps ship). A Developer ID adds the hardened
# runtime so the app can later be notarized without other changes.
SIGN_ARGS=(--force --sign "$SIGN_IDENTITY")
if [[ "$SIGN_IDENTITY" != "-" ]]; then SIGN_ARGS+=(--options runtime --timestamp); fi
for f in "${MACHO[@]}"; do
  [[ "$f" == "$APP/Contents/MacOS/mgr" ]] || codesign "${SIGN_ARGS[@]}" "$f"
done
codesign "${SIGN_ARGS[@]}" "$APP"
codesign --verify --deep --strict "$APP"

# --- Disk image --------------------------------------------------------------
DMG="$DIST/MGR-Recompiled-$VERSION-macos-arm64.dmg"
STAGE="$DIST/dmg-stage"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "$APP_NAME" -srcfolder "$STAGE" -fs HFS+ -format ULFO "$DMG"
rm -rf "$STAGE"
[[ "$SIGN_IDENTITY" == "-" ]] || codesign --force --sign "$SIGN_IDENTITY" --timestamp "$DMG"

echo "App:     $APP"
echo "Image:   $DMG"
echo "Symbols: $SYMBOLS"
shasum -a 256 "$DMG"
