#!/bin/bash
# Test, package and sign an update feed. Publishes nothing: it ends by printing
# the command that creates a draft GitHub release.
#
#   MGR_VERSION=0.1.0 scripts/release.sh
#
# Once per project, create the signing key (stored in your login keychain):
#   tools/sdk/sparkle/2.10.1/bin/generate_keys
# Put the printed public key in cmake/macos/sparkle-public-key.txt, and back up
# the private key (generate_keys -x <file>) somewhere outside this repository.
# Losing it means installed copies can no longer be updated.
#
# MGR_ED_KEY_FILE=<file> signs with a key file instead of the keychain.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${MGR_VERSION:?set MGR_VERSION, such as 0.1.0}"
SPARKLE="${MGR_SPARKLE_DIR:-$ROOT/tools/sdk/sparkle/2.10.1}"
REPO_URL="https://github.com/ginnfx/MGR-Recompiled"
NAME="MGR-Recompiled-$VERSION-macos-arm64.dmg"
FEED="$ROOT/dist/feed-$VERSION"

MGR_REQUIRE_UPDATER=1 "$ROOT/scripts/package-macos.sh"
cmake --build "$ROOT/build-release" --target mgr_tests --parallel "${MGR_BUILD_JOBS:-4}"
ctest --test-dir "$ROOT/build-release" --output-on-failure

rm -rf "$FEED" && mkdir -p "$FEED"
cp "$ROOT/dist/$NAME" "$FEED/"
KEY_ARGS=()
[[ -z "${MGR_ED_KEY_FILE:-}" ]] || KEY_ARGS=(--ed-key-file "$MGR_ED_KEY_FILE")
"$SPARKLE/bin/generate_appcast" ${KEY_ARGS[@]+"${KEY_ARGS[@]}"} \
  --download-url-prefix "$REPO_URL/releases/download/v$VERSION/" "$FEED"
grep -q 'sparkle:edSignature' "$FEED/appcast.xml" ||
  { echo "release: the update feed is unsigned (wrong or missing key)" >&2; exit 1; }

echo
echo "Ready in $FEED. After testing, create a draft release (this uploads the files):"
echo "  gh release create v$VERSION --draft --target \"$(git -C "$ROOT" rev-parse HEAD)\" \\"
echo "    \"$FEED/$NAME\" \"$FEED/appcast.xml\""
