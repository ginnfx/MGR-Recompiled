// Native renderer: texture fetch constant decode.

#include "hooks/texture_fetch.h"

#include <algorithm>
#include <cstring>

#include <format>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/math.h>

#include "core/memory_helpers.h"
#include "gpu/d3d.h"

namespace mgrn::native {

namespace {

namespace xe = rex::graphics::xenos;

// D3DFORMAT field positions (see gpu/format.cpp DecodeD3DFormat).
constexpr u32 kD3DFormatEndianShift = 6;
constexpr u32 kD3DFormatTiledShift = 8;
constexpr u32 kD3DFormatSignShift = 9;
// Sign fields of fetch dword 0 (X, Y, Z, W, two bits each from bit 2).
constexpr u32 kFetchSignShift = 2;
constexpr u32 kFetchSignMask = 0xFFu;
// Tiled textures are laid out in 32x32-block tiles, 3D ones in 4-slice groups.
constexpr u32 kTileBlocks = 32;
constexpr u32 kTileSlices = 4;
constexpr u32 kCubeFaces = 6;
constexpr u32 kLinearRowAlignment = 256;

u32 Mip0Bytes(const TextureFetchDesc& desc, u32 pitch_texels) {
  const auto* info = rex::graphics::FormatInfo::Get(desc.format);
  if (!info || !info->block_width) return 0;
  const u32 bw = info->block_width, bh = info->block_height ? info->block_height : bw;
  const u32 bpb = info->bytes_per_block();
  u32 blocks_w = (desc.width + bw - 1) / bw;
  u32 blocks_h = (desc.height + bh - 1) / bh;
  u32 slices = desc.dimension == xe::DataDimension::kCube ? kCubeFaces : desc.depth;
  u64 row_bytes;
  if (desc.tiled) {
    blocks_w = rex::align(blocks_w, kTileBlocks);
    blocks_h = rex::align(blocks_h, kTileBlocks);
    if (desc.dimension == xe::DataDimension::k3D) slices = rex::align(slices, kTileSlices);
    row_bytes = u64(blocks_w) * bpb;
  } else {
    row_bytes = pitch_texels ? u64(pitch_texels / bw) * bpb
                             : rex::align<u64>(u64(blocks_w) * bpb, kLinearRowAlignment);
  }
  const u64 total = row_bytes * blocks_h * slices;
  return total > UINT32_MAX ? UINT32_MAX : u32(total);
}

}  // namespace

bool DecodeTextureFetch(u32 texture_va, TextureFetchDesc& out) {
  const auto* guest = mgrn::mem::try_at<const gpu::D3DTexture>(texture_va);
  if (!guest) return false;
  u32 words[6];
  for (int i = 0; i < 6; ++i) words[i] = u32(guest->Format.dword[i]);
  xe::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  if (fetch.type != xe::FetchConstantType::kTexture) return false;

  TextureFetchDesc desc;
  desc.format = fetch.format;
  desc.dimension = fetch.dimension;
  desc.endian = fetch.endianness;
  desc.tiled = fetch.tiled != 0;
  desc.base_address = fetch.base_address << 12;
  switch (fetch.dimension) {
    case xe::DataDimension::k1D:
      desc.width = fetch.size_1d.width + 1;
      desc.height = 1;
      break;
    case xe::DataDimension::k3D:
      desc.width = fetch.size_3d.width + 1;
      desc.height = fetch.size_3d.height + 1;
      desc.depth = fetch.size_3d.depth + 1;
      break;
    default:
      desc.width = fetch.size_2d.width + 1;
      desc.height = fetch.size_2d.height + 1;
      break;
  }
  desc.levels = std::max<u32>(fetch.mip_max_level, fetch.mip_min_level) + 1;
  desc.size_bytes = Mip0Bytes(desc, u32(fetch.pitch) << 5);
  desc.d3d_format = u32(fetch.format) | (u32(fetch.endianness) << kD3DFormatEndianShift) |
                    (u32(fetch.tiled) << kD3DFormatTiledShift) |
                    (((words[0] >> kFetchSignShift) & kFetchSignMask) << kD3DFormatSignShift);
  out = desc;
  return true;
}

std::string DescribeFetch(const TextureFetchDesc& desc) {
  return std::format("fmt=0x{:X} dim={} {}x{}x{} levels={} base=0x{:08X} tiled={} endian={}",
                     u32(desc.format), u32(desc.dimension), desc.width, desc.height, desc.depth,
                     desc.levels, desc.base_address, desc.tiled ? 1 : 0, u32(desc.endian));
}

}  // namespace mgrn::native
