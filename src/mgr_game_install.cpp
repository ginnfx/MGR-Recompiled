#include "mgr_game_install.h"

#include "mgr_byte_order.h"

#include <xxhash.h>

#include <exception>
#include <fstream>
#include <system_error>
#include <vector>

namespace mgr_install {
namespace {

namespace fs = std::filesystem;

constexpr ManifestEntry kManifest[] = {
#include "mgr_game_manifest.inc"
};

constexpr size_t kChunkBytes = size_t{1} << 20;
// default.xex is read into memory whole; far above any real one, so a bogus
// file is refused before it is allocated.
constexpr int64_t kMaxXexBytes = int64_t{64} << 20;
// Room for the install marker and filesystem overhead beyond the file bytes.
constexpr uint64_t kSpaceMarginBytes = uint64_t{256} << 20;
constexpr uint32_t kExecutionInfoKey = 0x00040006;
constexpr size_t kXexHeaderCountOffset = 0x14;
constexpr size_t kXexHeadersOffset = 0x18;

const ManifestEntry* FindEntry(ManifestView manifest, std::string_view path) {
  for (const auto& entry : manifest)
    if (path == entry.path) return &entry;
  return nullptr;
}

struct HashState {
  HashState() : state(XXH3_createState()) { XXH3_64bits_reset(state); }
  ~HashState() { XXH3_freeState(state); }
  HashState(const HashState&) = delete;
  HashState& operator=(const HashState&) = delete;
  XXH3_state_t* state;
};

// Identifies the manifest a game folder was installed from.
std::string MarkerText(ManifestView manifest) {
  HashState hash;
  for (const auto& entry : manifest) {
    XXH3_64bits_update(hash.state, entry.path, std::string_view(entry.path).size() + 1);
    XXH3_64bits_update(hash.state, &entry.size, sizeof(entry.size));
    XXH3_64bits_update(hash.state, &entry.xxh3, sizeof(entry.xxh3));
  }
  return "mgr-install " + std::to_string(XXH3_64bits_digest(hash.state)) + "\n";
}

InstallResult Failure(std::string error) { return {false, false, std::move(error)}; }

// Copies one file into dest_dir while hashing it.
InstallResult CopyEntry(GameSource& source, const ManifestEntry& entry, const fs::path& dest_dir,
                        uint64_t& done, uint64_t total, const Progress& progress,
                        const std::atomic<bool>& cancel) {
  const int64_t size = source.Size(entry.path);
  if (size < 0) return Failure(std::string("The disc is missing ") + entry.path + ".");
  if (uint64_t(size) != entry.size)
    return Failure(std::string(entry.path) + " has the wrong size; the disc image may be damaged.");

  const fs::path dest = dest_dir / entry.path;
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (ec || !out) return Failure("Could not write " + dest.string() + ".");

  HashState hash;
  bool write_failed = false;
  const bool read_ok = source.Read(entry.path, [&](std::span<const uint8_t> chunk) {
    if (cancel.load(std::memory_order_relaxed)) return false;
    XXH3_64bits_update(hash.state, chunk.data(), chunk.size());
    out.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
    if (!out) {
      write_failed = true;
      return false;
    }
    done += chunk.size();
    if (progress) progress(done, total);
    return true;
  });
  out.close();
  if (cancel.load(std::memory_order_relaxed)) return {false, true, {}};
  if (write_failed || !out) return Failure("Could not write " + dest.string() + ".");
  if (!read_ok) return Failure(std::string("Could not read ") + entry.path + " from the disc.");
  if (XXH3_64bits_digest(hash.state) != entry.xxh3)
    return Failure(std::string(entry.path) + " does not match the supported disc; the image may be damaged.");
  return {true, false, {}};
}

// Moves the finished copy into place, keeping the old game/ until it succeeds.
InstallResult Commit(const fs::path& partial, const fs::path& game) {
  std::error_code ec;
  const fs::path old = game.string() + ".old";
  const bool had_game = fs::exists(game, ec);
  // With no game/, game.old may be all that is left of an interrupted swap, so
  // it stays until the new copy is in place.
  if (had_game) {
    fs::remove_all(old, ec);
    fs::rename(game, old, ec);
  } else {
    ec.clear();
  }
  if (!ec) fs::rename(partial, game, ec);
  if (ec) {
    const std::string reason = ec.message();
    std::error_code ignored;  // `reason` is what the player needs to see
    if (had_game && !fs::exists(game, ignored)) fs::rename(old, game, ignored);
    return Failure("Could not move the game files into place: " + reason);
  }
  fs::remove_all(old, ec);
  return {true, false, {}};
}

}  // namespace

ManifestView Manifest() { return kManifest; }

uint64_t TotalBytes(ManifestView manifest) {
  uint64_t total = 0;
  for (const auto& entry : manifest) total += entry.size;
  return total;
}

XexCheck CheckXex(std::span<const uint8_t> xex, uint64_t expected_xxh3) {
  if (xex.size() < kXexHeadersOffset || ReadBE32(xex.data()) != 0x58455832)  // "XEX2"
    return XexCheck::kNotXex;
  const uint32_t count = ReadBE32(xex.data() + kXexHeaderCountOffset);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = kXexHeadersOffset + size_t{i} * 8;
    if (at + 8 > xex.size()) break;
    if (ReadBE32(xex.data() + at) != kExecutionInfoKey) continue;
    const size_t info = ReadBE32(xex.data() + at + 4);
    if (info + 16 > xex.size()) return XexCheck::kNotXex;
    if (ReadBE32(xex.data() + info + 12) != kTitleId) return XexCheck::kOtherGame;
    if (ReadBE32(xex.data() + info) != kMediaId || ReadBE32(xex.data() + info + 4) != kVersion)
      return XexCheck::kOtherVersion;
    return XXH3_64bits(xex.data(), xex.size()) == expected_xxh3 ? XexCheck::kOk
                                                                : XexCheck::kModified;
  }
  return XexCheck::kNotXex;
}

const char* Describe(XexCheck check) {
  switch (check) {
    case XexCheck::kOk:
      return "This is the supported copy of Metal Gear Rising: Revengeance.";
    case XexCheck::kNotXex:
      return "This doesn't look like an Xbox 360 game. Choose a disc image (.iso) or a "
             "folder that contains default.xex.";
    case XexCheck::kOtherGame:
      return "This is a different Xbox 360 game. Choose Metal Gear Rising: Revengeance.";
    case XexCheck::kOtherVersion:
      return "This is another release of Metal Gear Rising: Revengeance. Only the Xbox 360 "
             "USA/Europe disc (title 4B4E080A, media ID 6CC18291) is supported.";
    case XexCheck::kModified:
      return "This copy's default.xex has been modified or is damaged. Use an unmodified "
             "dump of the disc.";
  }
  return "";
}

int64_t FolderSource::Size(std::string_view path) {
  std::error_code ec;
  const auto size = fs::file_size(root_ / path, ec);
  return ec ? -1 : int64_t(size);
}

bool FolderSource::Read(std::string_view path,
                        const std::function<bool(std::span<const uint8_t>)>& sink) {
  std::ifstream in(root_ / path, std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> buffer(kChunkBytes);
  while (in) {
    in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
    const auto got = size_t(in.gcount());
    if (got > 0 && !sink({buffer.data(), got})) return false;
  }
  return in.eof();
}

static XexCheck ReadAndCheckXex(GameSource& source, ManifestView manifest) {
  const ManifestEntry* entry = FindEntry(manifest, "default.xex");
  const int64_t size = source.Size("default.xex");
  if (!entry || size <= 0 || size > kMaxXexBytes) return XexCheck::kNotXex;
  std::vector<uint8_t> xex;
  xex.reserve(size_t(size));
  if (!source.Read("default.xex", [&](std::span<const uint8_t> chunk) {
        xex.insert(xex.end(), chunk.begin(), chunk.end());
        return true;
      }))
    return XexCheck::kNotXex;
  return CheckXex(xex, entry->xxh3);
}

XexCheck CheckSourceXex(GameSource& source, ManifestView manifest) {
  try {
    return ReadAndCheckXex(source, manifest);
  } catch (const std::exception&) {  // unreadable source or out of memory
    return XexCheck::kNotXex;
  }
}

static InstallResult InstallUnchecked(GameSource& source, const fs::path& data_root,
                                      const Progress& progress, const std::atomic<bool>& cancel,
                                      ManifestView manifest) {
  std::error_code ec;
  fs::create_directories(data_root, ec);
  if (ec) return Failure("Could not create " + data_root.string() + ": " + ec.message());
  const uint64_t total = TotalBytes(manifest);
  const fs::space_info space = fs::space(data_root, ec);
  if (!ec && space.available < total + kSpaceMarginBytes) {
    const auto need_gb = double(total + kSpaceMarginBytes) / 1e9;
    return Failure("Not enough free disk space. The game needs about " +
                   std::to_string(int(need_gb + 0.99)) + " GB.");
  }

  const fs::path partial = data_root / "game.partial";
  fs::remove_all(partial, ec);
  uint64_t done = 0;
  InstallResult result{true, false, {}};
  for (const auto& entry : manifest) {
    result = CopyEntry(source, entry, partial, done, total, progress, cancel);
    if (!result.ok) break;
  }
  if (result.ok) {
    std::ofstream marker(partial / kMarkerName, std::ios::trunc);
    marker << MarkerText(manifest);
    marker.close();
    if (!marker) result = Failure("Could not write the install marker.");
  }
  if (result.ok) result = Commit(partial, data_root / "game");
  if (!result.ok) fs::remove_all(partial, ec);
  return result;
}

// Callers run this on a worker thread, where an escaping exception would end
// the app, so every failure comes back as a result.
InstallResult InstallFrom(GameSource& source, const fs::path& data_root, const Progress& progress,
                          const std::atomic<bool>& cancel, ManifestView manifest) {
  try {
    return InstallUnchecked(source, data_root, progress, cancel, manifest);
  } catch (const std::exception& error) {
    std::error_code ignored;
    fs::remove_all(data_root / "game.partial", ignored);
    return Failure(std::string("Setup stopped unexpectedly: ") + error.what());
  }
}

bool IsInstalled(const fs::path& game_dir, ManifestView manifest) {
  std::ifstream marker(game_dir / kMarkerName);
  const std::string text((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
  if (text != MarkerText(manifest)) return false;
  std::error_code ec;
  for (const auto& entry : manifest) {
    if (fs::file_size(game_dir / entry.path, ec) != entry.size || ec) return false;
  }
  return true;
}

}  // namespace mgr_install
