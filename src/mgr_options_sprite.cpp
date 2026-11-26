#include "mgr_options_sprite.h"

#include "generated/default/mgr_pch.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>

namespace mgr_options_sprite {
namespace {

constexpr uint32_t kTextureSize = 512;
constexpr uint32_t kTextureBytes = kTextureSize * kTextureSize;  // DXT5: one byte per texel
// Sprite widget: +44 -> holder whose +4 is the loaded WTB buffer; +56 -> texture
// object (+8 width, +12 height, +20 byte size). All traced on 2026-11-26.
constexpr uint32_t kWidgetHolder = 44, kWidgetTexture = 56;
constexpr uint32_t kHolderBuffer = 4;
constexpr uint32_t kTextureWidth = 8, kTextureHeight = 12, kTextureByteSize = 20;
// "\0BTW" header: offset table location at +12 (first texture's data offset),
// size table location at +16.
constexpr uint32_t kWtbMagic = 0x00425457, kWtbOffsetTable = 12, kWtbSizeTable = 16;
// Texture data must lie within this many bytes of the WTB buffer start.
constexpr uint32_t kMaxTextureOffset = 0x1000000;
// Table offsets inside the WTB header stay within its first page.
constexpr uint32_t kWtbHeaderLimit = 0x1000;

uint32_t U32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, REX_RAW_ADDR(address), 4);
  return __builtin_bswap32(v);
}

}  // namespace

bool SpriteTexture::Attach(uint8_t* base, uint32_t widget) {
  Restore(base);
  const uint32_t holder = U32(base, widget + kWidgetHolder), texture = U32(base, widget + kWidgetTexture);
  if (!holder || !texture) return false;
  const uint32_t buffer = U32(base, holder + kHolderBuffer);
  if (!buffer || U32(base, buffer) != kWtbMagic) {
    std::fprintf(stderr, "[mgr-options-sprite] no WTB buffer (holder=%08X buffer=%08X)\n", holder, buffer);
    return false;
  }
  const uint32_t offset_table = U32(base, buffer + kWtbOffsetTable), size_table = U32(base, buffer + kWtbSizeTable);
  if (offset_table > kWtbHeaderLimit - 4 || size_table > kWtbHeaderLimit - 4) {
    std::fprintf(stderr, "[mgr-options-sprite] bad WTB tables %08X %08X\n", offset_table, size_table);
    return false;
  }
  const uint32_t width = U32(base, texture + kTextureWidth), height = U32(base, texture + kTextureHeight);
  const uint32_t first_offset = U32(base, buffer + offset_table);
  const uint32_t first_size = U32(base, buffer + size_table);
  if (width != kTextureSize || height != kTextureSize || first_size != kTextureBytes ||
      U32(base, texture + kTextureByteSize) != kTextureBytes || first_offset > kMaxTextureOffset - first_size) {
    std::fprintf(stderr, "[mgr-options-sprite] unexpected texture %ux%u size=%u offset=%08X\n", width, height,
                 first_size, first_offset);
    return false;
  }
  buffer_ = buffer;
  data_ = buffer + first_offset;
  original_.assign(REX_RAW_ADDR(data_), REX_RAW_ADDR(data_) + kTextureBytes);
  written_.clear();
  std::fprintf(stderr, "[mgr-options-sprite] attached texture data=%08X\n", data_);
  // MGR_OPTIONS_DUMP_TEXTURE=<file> (diagnostic): the raw tiled texture, to decode offline.
  if (const char* path = std::getenv("MGR_OPTIONS_DUMP_TEXTURE")) DumpTexture(path);
  return true;
}

void SpriteTexture::DumpTexture(const char* path) const {
  std::FILE* out = std::fopen(path, "wb");
  if (!out) {
    std::fprintf(stderr, "[mgr-options-sprite] cannot open %s\n", path);
    return;
  }
  if (std::fwrite(original_.data(), 1, original_.size(), out) != original_.size())
    std::fprintf(stderr, "[mgr-options-sprite] short write to %s\n", path);
  std::fclose(out);
}

void SpriteTexture::Write(uint8_t* base, const std::vector<uint8_t>& rgba, mgr_pictures::Region region) {
  if (!data_) return;
  try {
    mgr_pictures::EncodeTiledDxt5Region(rgba, kTextureSize, kTextureSize, region, REX_RAW_ADDR(data_));
    const bool known = std::any_of(written_.begin(), written_.end(), [&](const mgr_pictures::Region& r) {
      return r.x == region.x && r.y == region.y && r.width == region.width && r.height == region.height;
    });
    if (!known) written_.push_back(region);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "[mgr-options-sprite] encode failed: %s\n", error.what());
  }
}

void SpriteTexture::Forget() {
  buffer_ = data_ = 0;
  original_.clear();
  written_.clear();
}

void SpriteTexture::Restore(uint8_t* base) {
  if (!data_) return;
  // The texture memory may have been freed or reused since Attach (stale session):
  // only write back while the WTB buffer is still the one we attached to.
  if (U32(base, buffer_) != kWtbMagic) {
    std::fprintf(stderr, "[mgr-options-sprite] buffer %08X is gone, not restoring\n", buffer_);
    Forget();
    return;
  }
  // Only the blocks of the written regions differ from the original.
  for (const auto& region : written_)
    mgr_pictures::CopyTiledRegion(original_.data(), REX_RAW_ADDR(data_), kTextureSize, kTextureSize, region);
  Forget();
}

}  // namespace mgr_options_sprite
