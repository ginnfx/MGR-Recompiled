// Paths for the packaged MGR Recompiled.app. A signed bundle is read-only, so
// game files, saves, caches, logs and mgr.toml live under Application Support;
// dev builds (scripts/run.sh from build/) keep their command-line paths.
#pragma once

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace mgr_bundle {

inline constexpr const char* kDataFolderName = "MGR-Recompiled";

// exe_dir without a trailing separator, so parent_path() steps up one level.
inline std::filesystem::path Normalized(const std::filesystem::path& exe_dir) {
  auto dir = exe_dir.lexically_normal();
  return dir.has_filename() ? dir : dir.parent_path();
}

inline bool IsAppBundle(const std::filesystem::path& exe_dir) {
  const auto macos = Normalized(exe_dir);
  const auto contents = macos.parent_path();
  return macos.filename() == "MacOS" && contents.filename() == "Contents" &&
         contents.parent_path().extension() == ".app";
}

inline std::filesystem::path ResourcesDir(const std::filesystem::path& exe_dir) {
  return Normalized(exe_dir).parent_path() / "Resources";
}

inline std::filesystem::path HomeDir() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  return {};
}

inline std::filesystem::path DataRoot(const std::filesystem::path& home) {
  return home / "Library" / "Application Support" / kDataFolderName;
}

struct DataLayout {
  std::filesystem::path game;
  std::filesystem::path user;
  std::filesystem::path cache;
  std::filesystem::path logs;
  std::filesystem::path config;
  std::filesystem::path dlc;  // DLC packages waiting to be unpacked
};

inline DataLayout Layout(const std::filesystem::path& root) {
  return {root / "game", root / "user", root / "cache", root / "logs", root / "mgr.toml", root / "dlc"};
}

}  // namespace mgr_bundle
