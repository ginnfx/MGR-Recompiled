#include "mgr_disc_source.h"

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/logging.h>

#include <algorithm>

namespace mgr_install {
namespace {

using rex::filesystem::DiscImageDevice;
using rex::filesystem::DiscImageEntry;

constexpr size_t kChunkBytes = size_t{1} << 20;

class DiscImageSource final : public GameSource {
 public:
  explicit DiscImageSource(std::unique_ptr<DiscImageDevice> device) : device_(std::move(device)) {}

  int64_t Size(std::string_view path) override {
    const auto* entry = Find(path);
    return entry ? int64_t(entry->data_size()) : -1;
  }

  bool Read(std::string_view path,
            const std::function<bool(std::span<const uint8_t>)>& sink) override {
    const auto* entry = Find(path);
    if (!entry) return false;
    const uint8_t* data = entry->mmap()->data() + entry->data_offset();
    for (size_t at = 0; at < entry->data_size(); at += kChunkBytes) {
      const size_t length = std::min(kChunkBytes, entry->data_size() - at);
      if (!sink({data + at, length})) return false;
    }
    return true;
  }

 private:
  // Every entry of a disc image device is a DiscImageEntry. Entries whose data
  // runs past the end of the image (a truncated download) count as missing,
  // so reading them can never fault.
  const DiscImageEntry* Find(std::string_view path) {
    const auto* entry = static_cast<const DiscImageEntry*>(device_->ResolvePath(path));
    if (!entry || !entry->mmap()) return nullptr;
    const size_t image_size = entry->mmap()->size();
    if (entry->data_offset() > image_size || entry->data_size() > image_size - entry->data_offset())
      return nullptr;
    return entry;
  }

  std::unique_ptr<DiscImageDevice> device_;
};

}  // namespace

std::unique_ptr<GameSource> OpenDiscImage(const std::filesystem::path& iso) {
  auto device = std::make_unique<DiscImageDevice>("\\Device\\Cdrom0", iso);
  if (!device->Initialize()) {
    REXLOG_ERROR("Setup: {} is not a readable Xbox 360 disc image", iso.string());
    return nullptr;
  }
  REXLOG_INFO("Setup: opened disc image {} ({} files)", iso.string(), device->file_count());
  return std::make_unique<DiscImageSource>(std::move(device));
}

}  // namespace mgr_install
