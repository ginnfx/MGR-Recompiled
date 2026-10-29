#include "mgr_game_install.h"

#include <xxhash.h>

#include <unistd.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using mgr_install::XexCheck;

namespace {

std::vector<uint8_t> FakeXex(uint32_t media_id, uint32_t version, uint32_t title_id) {
  std::vector<uint8_t> xex(0x40, 0);
  auto put = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) xex[at + i] = uint8_t(v >> (24 - 8 * i));
  };
  put(0x00, 0x58455832);  // "XEX2"
  put(0x14, 1);           // one optional header
  put(0x18, 0x00040006);  // execution info ...
  put(0x1C, 0x20);        // ... at 0x20
  put(0x20, media_id);
  put(0x24, version);
  put(0x2C, title_id);
  return xex;
}

std::vector<uint8_t> SupportedXex() {
  return FakeXex(mgr_install::kMediaId, mgr_install::kVersion, mgr_install::kTitleId);
}

uint64_t Hash(const std::vector<uint8_t>& bytes) { return XXH3_64bits(bytes.data(), bytes.size()); }

void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> Bytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(seed + i * 7);
  return bytes;
}

struct Fixture {
  static inline int next_id = 0;
  fs::path dir = fs::temp_directory_path() /
                 ("mgr_install_test_" + std::to_string(getpid()) + "_" + std::to_string(next_id++));
  fs::path source = dir / "disc";
  fs::path data = dir / "data";
  std::vector<uint8_t> xex = SupportedXex();
  std::vector<uint8_t> movie = Bytes(3 << 20, 1);  // spans several read chunks
  std::vector<uint8_t> sound = Bytes(1000, 9);
  std::vector<mgr_install::ManifestEntry> manifest;

  Fixture() {
    fs::remove_all(dir);
    WriteFile(source / "default.xex", xex);
    WriteFile(source / "media/movie/ev0000.usm", movie);
    WriteFile(source / "media/sound/Init.bnk", sound);
    manifest = {{"default.xex", xex.size(), Hash(xex)},
                {"media/movie/ev0000.usm", movie.size(), Hash(movie)},
                {"media/sound/Init.bnk", sound.size(), Hash(sound)}};
  }
  ~Fixture() { fs::remove_all(dir); }

  mgr_install::InstallResult Install(const std::atomic<bool>& cancel = false_flag) {
    mgr_install::FolderSource folder(source);
    return mgr_install::InstallFrom(folder, data, {}, cancel, manifest);
  }
  bool Installed() { return mgr_install::IsInstalled(data / "game", manifest); }

  static inline const std::atomic<bool> false_flag{false};
};

void TestCheckXex() {
  const auto xex = SupportedXex();
  assert(mgr_install::CheckXex(xex, Hash(xex)) == XexCheck::kOk);
  assert(mgr_install::CheckXex(xex, Hash(xex) + 1) == XexCheck::kModified);

  auto bad_magic = xex;
  bad_magic[0] = 'Z';
  assert(mgr_install::CheckXex(bad_magic, Hash(bad_magic)) == XexCheck::kNotXex);
  assert(mgr_install::CheckXex({xex.data(), 0x10}, 0) == XexCheck::kNotXex);
  auto info_out_of_range = xex;
  info_out_of_range[0x1F] = 0xF0;
  assert(mgr_install::CheckXex(info_out_of_range, 0) == XexCheck::kNotXex);

  const auto other_game = FakeXex(mgr_install::kMediaId, mgr_install::kVersion, 0x41560001);
  assert(mgr_install::CheckXex(other_game, Hash(other_game)) == XexCheck::kOtherGame);
  const auto other_disc = FakeXex(0x12345678, mgr_install::kVersion, mgr_install::kTitleId);
  assert(mgr_install::CheckXex(other_disc, Hash(other_disc)) == XexCheck::kOtherVersion);
  const auto other_version = FakeXex(mgr_install::kMediaId, 5, mgr_install::kTitleId);
  assert(mgr_install::CheckXex(other_version, Hash(other_version)) == XexCheck::kOtherVersion);
}

void TestCheckSourceXex() {
  Fixture f;
  mgr_install::FolderSource folder(f.source);
  assert(mgr_install::CheckSourceXex(folder, f.manifest) == XexCheck::kOk);
  mgr_install::FolderSource empty(f.dir / "nothing-here");
  assert(mgr_install::CheckSourceXex(empty, f.manifest) == XexCheck::kNotXex);
}

void TestInstallAndReport() {
  Fixture f;
  mgr_install::FolderSource folder(f.source);
  uint64_t last_done = 0, last_total = 0;
  const std::atomic<bool> cancel{false};
  const auto result = mgr_install::InstallFrom(
      folder, f.data, [&](uint64_t done, uint64_t total) { last_done = done, last_total = total; },
      cancel, f.manifest);
  assert(result.ok && !result.cancelled && result.error.empty());
  assert(last_total == mgr_install::TotalBytes(f.manifest) && last_done == last_total);
  assert(fs::file_size(f.data / "game/media/movie/ev0000.usm") == f.movie.size());
  assert(!fs::exists(f.data / "game.partial"));
  assert(f.Installed());
}

void TestMissingFileKeepsOldInstall() {
  Fixture f;
  assert(f.Install().ok);
  fs::remove(f.source / "media/sound/Init.bnk");
  const auto result = f.Install();
  assert(!result.ok && !result.cancelled);
  assert(result.error.find("missing media/sound/Init.bnk") != std::string::npos);
  assert(!fs::exists(f.data / "game.partial"));
  assert(f.Installed());  // the earlier install is untouched
}

void TestHashMismatchRollsBack() {
  Fixture f;
  auto damaged = f.sound;
  damaged[10] ^= 0xFF;
  WriteFile(f.source / "media/sound/Init.bnk", damaged);
  const auto result = f.Install();
  assert(!result.ok && result.error.find("does not match") != std::string::npos);
  assert(!fs::exists(f.data / "game.partial") && !fs::exists(f.data / "game"));
}

void TestWrongSizeRejected() {
  Fixture f;
  WriteFile(f.source / "media/sound/Init.bnk", Bytes(999, 9));
  const auto result = f.Install();
  assert(!result.ok && result.error.find("wrong size") != std::string::npos);
  assert(!fs::exists(f.data / "game"));
}

void TestCancel() {
  Fixture f;
  const std::atomic<bool> cancel{true};
  const auto result = f.Install(cancel);
  assert(!result.ok && result.cancelled && result.error.empty());
  assert(!fs::exists(f.data / "game.partial") && !fs::exists(f.data / "game"));
}

void TestIsInstalledDetectsDamage() {
  Fixture f;
  assert(!f.Installed());  // nothing installed yet
  assert(f.Install().ok);
  fs::resize_file(f.data / "game/media/movie/ev0000.usm", 10);
  assert(!f.Installed());

  Fixture g;
  assert(g.Install().ok);
  auto newer = g.manifest;
  newer[2].xxh3 ^= 1;  // a build with a different manifest must reinstall
  assert(!mgr_install::IsInstalled(g.data / "game", newer));
}

void TestRealManifestShape() {
  const auto manifest = mgr_install::Manifest();
  assert(!manifest.empty() && std::string_view(manifest[0].path) == "default.xex");
  for (const auto& entry : manifest) {
    const std::string_view path = entry.path;
    assert(entry.size > 0);
    assert(path == "default.xex" || path.starts_with("media/"));
    assert(path.find("..") == std::string_view::npos && !path.starts_with("/"));
  }
}

}  // namespace

int main() {
  TestCheckXex();
  TestCheckSourceXex();
  TestInstallAndReport();
  TestMissingFileKeepsOldInstall();
  TestHashMismatchRollsBack();
  TestWrongSizeRejected();
  TestCancel();
  TestIsInstalledDetectsDamage();
  TestRealManifestShape();
  std::cout << "game_install: all checks passed\n";
  return 0;
}
