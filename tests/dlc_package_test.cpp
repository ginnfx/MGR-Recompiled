#include "mgr_game_install.h"

#include <CommonCrypto/CommonDigest.h>
#include <unistd.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using mgr_install::DlcCheck;

namespace {

constexpr size_t kBlock = 0x1000;
constexpr size_t kBase = 0xA000;  // header_size 0x9720 rounded up
constexpr uint32_t kDataBlocks = 5;

void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (24 - 8 * i));
}

// A minimal read-only LIVE package: one hash table, then kDataBlocks blocks.
std::vector<uint8_t> FakePackage(uint32_t title = mgr_install::kTitleId, uint32_t type = 2) {
  std::vector<uint8_t> b(kBase + (1 + kDataBlocks) * kBlock, 0);
  Put32(b, 0, 0x4C495645);  // "LIVE"
  Put32(b, 0x340, 0x9720);
  Put32(b, 0x344, type);
  Put32(b, 0x360, title);
  b[0x379] = 0x24;           // descriptor length
  b[0x379 + 2] = 1;          // read-only format
  Put32(b, 0x379 + 0x1C, kDataBlocks);
  const std::u16string name = u"Wolf's Side Story";
  for (size_t i = 0; i < name.size(); ++i) b[0x411 + 2 * i + 1] = uint8_t(name[i]);
  for (uint32_t block = 0; block < kDataBlocks; ++block) {
    uint8_t* data = &b[kBase + (block + 1) * kBlock];
    for (size_t i = 0; i < kBlock; ++i) data[i] = uint8_t(block * 31 + i);
    CC_SHA1(data, CC_LONG(kBlock), &b[kBase + block * 0x18]);
  }
  return b;
}

fs::path Write(const std::vector<uint8_t>& bytes, const std::string& name) {
  const fs::path path = fs::temp_directory_path() / ("mgr_dlc_test_" + std::to_string(getpid()) + name);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                              std::streamsize(bytes.size()));
  return path;
}

DlcCheck Check(const std::vector<uint8_t>& bytes) {
  const fs::path path = Write(bytes, "pkg");
  const auto result = mgr_install::CheckDlcPackage(path).check;
  fs::remove(path);
  return result;
}

}  // namespace

int main() {
  const auto good = FakePackage();
  const fs::path path = Write(good, "good");
  const auto info = mgr_install::CheckDlcPackage(path);
  assert(info.check == DlcCheck::kOk);
  assert(info.name == "Wolf's Side Story");

  auto damaged = good;
  damaged[kBase + 3 * kBlock + 100] ^= 0xFF;  // one byte in data block 2
  assert(Check(damaged) == DlcCheck::kDamaged);
  auto truncated = good;
  truncated.resize(truncated.size() - kBlock);
  assert(Check(truncated) == DlcCheck::kDamaged);
  assert(Check(FakePackage(0x5345085E)) == DlcCheck::kOtherGame);
  assert(Check(FakePackage(mgr_install::kTitleId, 1)) == DlcCheck::kNotDlc);
  auto writable = good;
  writable[0x379 + 2] = 0;
  assert(Check(writable) == DlcCheck::kUnsupported);
  auto not_package = good;
  not_package[0] = 'X';
  assert(Check(not_package) == DlcCheck::kNotPackage);
  assert(Check(std::vector<uint8_t>(64, 0)) == DlcCheck::kNotPackage);

  // Queueing copies the package under its own name into data_root/dlc.
  const fs::path root = fs::temp_directory_path() / ("mgr_dlc_root_" + std::to_string(getpid()));
  assert(mgr_install::QueueDlcPackage(path, root).ok);
  assert(fs::file_size(root / "dlc" / path.filename()) == good.size());
  assert(!fs::exists(root / "dlc" / (path.filename().string() + ".partial")));
  fs::remove_all(root);
  fs::remove(path);
  std::cout << "dlc_package: all checks passed\n";
  return 0;
}
