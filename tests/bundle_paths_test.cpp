#include "mgr_bundle_paths.h"

#include <cassert>
#include <iostream>

int main() {
  namespace fs = std::filesystem;

  // Only Contents/MacOS of a .app counts as a bundle; dev builds run from build/.
  assert(mgr_bundle::IsAppBundle("/Applications/MGR Recompiled.app/Contents/MacOS"));
  assert(mgr_bundle::IsAppBundle("/Applications/MGR Recompiled.app/Contents/MacOS/"));
  assert(!mgr_bundle::IsAppBundle("/Users/me/macgrpc/build"));
  assert(!mgr_bundle::IsAppBundle("/Users/me/Contents/MacOS"));
  assert(!mgr_bundle::IsAppBundle("/Applications/X.app/Contents/Resources"));
  assert(!mgr_bundle::IsAppBundle(""));

  assert(mgr_bundle::ResourcesDir("/A/X.app/Contents/MacOS") == fs::path("/A/X.app/Contents/Resources"));
  assert(mgr_bundle::ResourcesDir("/A/X.app/Contents/MacOS/") == fs::path("/A/X.app/Contents/Resources"));

  // Everything the app writes lives outside the (read-only, signed) bundle.
  const fs::path root = mgr_bundle::DataRoot("/Users/me");
  assert(root == fs::path("/Users/me/Library/Application Support/MGR-Recompiled"));
  const auto layout = mgr_bundle::Layout(root);
  assert(layout.game == root / "game");
  assert(layout.user == root / "user");
  assert(layout.cache == root / "cache");
  assert(layout.logs == root / "logs");
  assert(layout.config == root / "mgr.toml");

  std::cout << "bundle_paths: all checks passed\n";
  return 0;
}
