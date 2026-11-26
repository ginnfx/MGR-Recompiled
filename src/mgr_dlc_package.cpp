// Xbox Live content package (STFS) check for the setup's DLC step.
#include "mgr_game_install.h"

#include "mgr_byte_order.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <system_error>

namespace mgr_install {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagicLive = 0x4C495645;  // "LIVE"
constexpr uint32_t kMagicPirs = 0x50495253;  // "PIRS"
constexpr uint32_t kMarketplaceContent = 2;
constexpr size_t kHeaderSizeOffset = 0x340;
constexpr size_t kContentTypeOffset = 0x344;
constexpr size_t kTitleIdOffset = 0x360;
constexpr size_t kDescriptorOffset = 0x379;
constexpr size_t kVolumeTypeOffset = 0x3A9;
constexpr size_t kDisplayNameOffset = 0x411;
constexpr size_t kDisplayNameBytes = 0x80;
constexpr size_t kMinHeaderBytes = 0x971A;
constexpr size_t kBlockSize = 0x1000;
constexpr uint64_t kBlocksPerTable = 170;
constexpr uint64_t kBlocksPerLevel1 = kBlocksPerTable * kBlocksPerTable;
constexpr size_t kHashEntrySize = 0x18;

// Read-only packages keep one hash table per group; these mirror the SDK's
// block placement (stfs_container_device.cpp) for that layout.
uint64_t DataBlockPosition(uint64_t block) {
  uint64_t position = block;
  uint64_t span = kBlocksPerTable;
  for (int level = 0; level < 3; ++level) {
    position += (block + span) / span;
    if (block < span) break;
    span *= kBlocksPerTable;
  }
  return position;
}

uint64_t Level0TablePosition(uint64_t block) {
  if (block < kBlocksPerTable) return 0;
  const uint64_t position =
      (block / kBlocksPerTable) * (kBlocksPerTable + 1) + block / kBlocksPerLevel1 + 1;
  return block < kBlocksPerLevel1 ? position : position + 1;
}

std::string DisplayName(const uint8_t* utf16be) {
  std::string name;
  for (size_t i = 0; i + 1 < kDisplayNameBytes; i += 2) {
    const uint32_t c = uint32_t{utf16be[i]} << 8 | utf16be[i + 1];
    if (c == 0) break;
    if (c < 0x80) {
      name += char(c);
    } else if (c < 0x800) {
      name += char(0xC0 | c >> 6);
      name += char(0x80 | (c & 0x3F));
    } else {
      name += char(0xE0 | c >> 12);
      name += char(0x80 | (c >> 6 & 0x3F));
      name += char(0x80 | (c & 0x3F));
    }
  }
  return name;
}

struct Mapping {
  explicit Mapping(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end > 0) {
      void* mapped = mmap(nullptr, size_t(end), PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapped != MAP_FAILED) {
        data = static_cast<const uint8_t*>(mapped);
        size = size_t(end);
      }
    }
    close(fd);
  }
  ~Mapping() {
    if (data) munmap(const_cast<uint8_t*>(data), size);
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

bool VerifyBlocks(const Mapping& file, size_t base, uint64_t total_blocks) {
  for (uint64_t block = 0; block < total_blocks; ++block) {
    const uint64_t table = base + Level0TablePosition(block) * kBlockSize;
    const uint64_t data = base + DataBlockPosition(block) * kBlockSize;
    if (table + kBlockSize > file.size || data + kBlockSize > file.size) return false;
    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(file.data + data, CC_LONG(kBlockSize), digest);
    const uint8_t* expected = file.data + table + (block % kBlocksPerTable) * kHashEntrySize;
    if (std::memcmp(digest, expected, sizeof(digest)) != 0) return false;
  }
  return true;
}

}  // namespace

DlcPackage CheckDlcPackage(const fs::path& package) {
  DlcPackage result;
  const Mapping file(package);
  if (!file.data || file.size < kMinHeaderBytes) return result;
  const uint8_t* h = file.data;
  const uint32_t magic = ReadBE32(h);
  if (magic != kMagicLive && magic != kMagicPirs) return result;
  result.size = file.size;
  result.name = DisplayName(h + kDisplayNameOffset);
  if (ReadBE32(h + kTitleIdOffset) != kTitleId) return {DlcCheck::kOtherGame, result.name, result.size};
  if (ReadBE32(h + kContentTypeOffset) != kMarketplaceContent)
    return {DlcCheck::kNotDlc, result.name, result.size};

  const uint8_t* descriptor = h + kDescriptorOffset;
  const bool read_only = descriptor[2] & 1;
  if (ReadBE32(h + kVolumeTypeOffset) != 0 || !read_only)
    return {DlcCheck::kUnsupported, result.name, result.size};
  const uint32_t header_size = ReadBE32(h + kHeaderSizeOffset);
  const size_t base = (size_t(header_size) + kBlockSize - 1) & ~(kBlockSize - 1);
  const uint32_t total_blocks = ReadBE32(descriptor + 0x1C);
  // With no blocks the loop below would verify nothing and pass.
  if (total_blocks == 0) return {DlcCheck::kDamaged, result.name, result.size};
  result.check = VerifyBlocks(file, base, total_blocks) ? DlcCheck::kOk : DlcCheck::kDamaged;
  return result;
}

const char* Describe(DlcCheck check) {
  switch (check) {
    case DlcCheck::kOk:
      return "Good to go.";
    case DlcCheck::kNotPackage:
      return "That's not an Xbox Live download. DLC packages are files with long hex names.";
    case DlcCheck::kOtherGame:
      return "That package is for a different game. MGR:R DLC has title ID 4B4E080A.";
    case DlcCheck::kNotDlc:
      return "That's MGR:R content, but not DLC (probably a save or a theme).";
    case DlcCheck::kUnsupported:
      return "That package uses a layout setup can't read yet.";
    case DlcCheck::kDamaged:
      return "That package is damaged. Some of it doesn't match its own checksums, so grab a "
             "fresh copy.";
  }
  return "";
}

InstallResult QueueDlcPackage(const fs::path& package, const fs::path& data_root) {
  std::error_code ec;
  const fs::path dir = data_root / "dlc";
  fs::create_directories(dir, ec);
  const fs::path partial = dir / (package.filename().string() + ".partial");
  if (!ec) fs::copy_file(package, partial, fs::copy_options::overwrite_existing, ec);
  if (!ec) fs::rename(partial, dir / package.filename(), ec);
  if (ec) {
    const std::string reason = ec.message();
    fs::remove(partial, ec);
    return {false, false, "Could not copy the DLC package: " + reason};
  }
  return {true, false, {}};
}

std::filesystem::path DamagedPackagePath(const std::filesystem::path& package) {
  std::filesystem::path aside = package;
  aside += ".damaged";
  return aside;
}

bool IsPendingPackageFile(const std::filesystem::path& package) {
  const auto ext = package.extension();
  return ext != ".partial" && ext != ".damaged";
}

}  // namespace mgr_install
