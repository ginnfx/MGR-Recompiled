// Verified install of the player's game files into the app's data folder.
// Checks the disc's default.xex, then copies every manifest file (default.xex
// and media/) through a hash check into game.partial/, renaming it to game/
// only when all files match. No UI code; the setup window drives it.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace mgr_install {

struct ManifestEntry {
  const char* path;  // relative to the game root, '/'-separated
  uint64_t size;
  uint64_t xxh3;
};

using ManifestView = std::span<const ManifestEntry>;
// The supported disc's files (src/mgr_game_manifest.inc). Tests pass their own.
ManifestView Manifest();
[[nodiscard]] uint64_t TotalBytes(ManifestView manifest);

// Where the supported xex identifies itself (XEX2 execution info).
inline constexpr uint32_t kTitleId = 0x4B4E080A;
inline constexpr uint32_t kMediaId = 0x6CC18291;
inline constexpr uint32_t kVersion = 4;

enum class XexCheck {
  kOk,
  kNotXex,        // not an Xbox 360 executable
  kOtherGame,     // a different title ID
  kOtherVersion,  // MGR:R, but another region, disc or version
  kModified,      // right IDs, different bytes (patched or damaged)
};
// expected_xxh3 is the manifest hash of default.xex.
[[nodiscard]] XexCheck CheckXex(std::span<const uint8_t> xex, uint64_t expected_xxh3);
const char* Describe(XexCheck check);

// A place to read the game files from: an extracted folder or a disc image.
class GameSource {
 public:
  virtual ~GameSource() = default;
  // Size of the file at a manifest path, or -1 if it is missing.
  virtual int64_t Size(std::string_view path) = 0;
  // Passes the file's bytes to sink in order; false on a read error or when
  // sink returns false.
  virtual bool Read(std::string_view path,
                    const std::function<bool(std::span<const uint8_t>)>& sink) = 0;
};

class FolderSource final : public GameSource {
 public:
  explicit FolderSource(std::filesystem::path root) : root_(std::move(root)) {}
  int64_t Size(std::string_view path) override;
  bool Read(std::string_view path,
            const std::function<bool(std::span<const uint8_t>)>& sink) override;

 private:
  std::filesystem::path root_;
};

// Reads the source's default.xex fully and checks it.
[[nodiscard]] XexCheck CheckSourceXex(GameSource& source, ManifestView manifest = Manifest());

struct InstallResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;  // shown to the player when !ok && !cancelled
};

using Progress = std::function<void(uint64_t done, uint64_t total)>;

// Installs into data_root/game. Leaves an existing game/ in place until the new
// copy is complete; on failure or cancel only data_root/game.partial is removed.
[[nodiscard]] InstallResult InstallFrom(GameSource& source, const std::filesystem::path& data_root,
                          const Progress& progress, const std::atomic<bool>& cancel,
                          ManifestView manifest = Manifest());

// Quick check on launch: the install marker matches this build's manifest and
// every file has its expected size.
[[nodiscard]] bool IsInstalled(const std::filesystem::path& game_dir, ManifestView manifest = Manifest());

inline constexpr const char* kMarkerName = ".mgr-install";

// Xbox Live DLC packages (STFS "LIVE"/"PIRS"). The check reads the header and
// SHA-1 checks every data block, so a damaged copy is refused before the SDK
// ever unpacks it.
enum class DlcCheck {
  kOk,
  kNotPackage,   // not an Xbox 360 content package
  kOtherGame,    // a package for another title
  kNotDlc,       // a save, theme or other non-DLC content for this title
  kUnsupported,  // a package layout the checker doesn't handle
  kDamaged,      // blocks fail their hashes (bad copy or download)
};
struct DlcPackage {
  DlcCheck check = DlcCheck::kNotPackage;
  std::string name;      // English display name from the package header
  uint64_t size = 0;
};
[[nodiscard]] DlcPackage CheckDlcPackage(const std::filesystem::path& package);
const char* Describe(DlcCheck check);

// A pending package that fails its block hashes is renamed to <name>.damaged so
// later launches skip it instead of retrying.
std::filesystem::path DamagedPackagePath(const std::filesystem::path& package);
// False for files the installer must leave alone (.partial copies, .damaged).
[[nodiscard]] bool IsPendingPackageFile(const std::filesystem::path& package);

// Copies a checked package into data_root/dlc (unpacked on the next boot by
// MgrInstallPendingDlc).
[[nodiscard]] InstallResult QueueDlcPackage(const std::filesystem::path& package,
                              const std::filesystem::path& data_root);

}  // namespace mgr_install
