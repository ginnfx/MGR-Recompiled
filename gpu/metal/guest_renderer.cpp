#define XXH_INLINE_ALL
#include "guest_renderer.h"
#include "texture_copy.h"
#include "texture_swizzle.h"
#include "primitive_expansion.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <xxhash.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace mgr::gpu::metal {
using namespace rex::graphics;
namespace {
void Need(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
void Need(bool condition, const std::string& reason) {
  if (!condition) throw std::runtime_error(reason);
}
template<class T> std::span<const std::byte> Bytes(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}
uint32_t EnvU32(const char* name, uint32_t fallback) {
  const char* value = std::getenv(name);
  return value && *value ? uint32_t(std::strtoul(value, nullptr, 10)) : fallback;
}
inline int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                            uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (bytes_per_block_log2 + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bytes_per_block_log2 + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (bytes_per_block_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  pitch = (pitch + 31) & ~31u;
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytes_per_block_log2 + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytes_per_block_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}
struct ScopeTimer {
  uint64_t& total;
  std::chrono::steady_clock::time_point begin;
  explicit ScopeTimer(uint64_t& out) : total(out), begin(std::chrono::steady_clock::now()) {}
  ~ScopeTimer() {
    total += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
  }
};
uint32_t CountNonzeroDwords(const uint8_t* data, size_t size) {
  uint32_t count = 0;
  for (size_t i = 0; i + 4 <= size; i += 4) {
    uint32_t word;
    std::memcpy(&word, data + i, 4);
    count += word != 0;
  }
  return count;
}
std::array<float, 4> UnpackColor(xenos::ColorRenderTargetFormat format, const uint8_t* data) {
  uint32_t words[2]{}; std::memcpy(words, data, xenos::IsColorRenderTargetFormat64bpp(format) ? 8 : 4);
  std::array<float, 4> result{0,0,0,1};
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (uint32_t i = 0; i < 4; ++i) result[i] = float((words[0] >> (i * 8)) & 255) / 255.0f; break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t i = 0; i < 3; ++i) result[i] = float((words[0] >> (i * 10)) & 1023) / 1023.0f;
      result[3] = float(words[0] >> 30) / 3.0f; break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t i = 0; i < 3; ++i) result[i] = xenos::Float7e3To32((words[0] >> (i * 10)) & 1023);
      result[3] = float(words[0] >> 30) / 3.0f; break;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      for (uint32_t i = 0; i < (xenos::IsColorRenderTargetFormat64bpp(format) ? 4u : 2u); ++i)
        result[i] = rex::xenos_half_to_float(uint16_t(words[i / 2] >> ((i & 1) * 16)), true); break;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      for (uint32_t i = 0; i < (xenos::IsColorRenderTargetFormat64bpp(format) ? 4u : 2u); ++i)
        result[i] = std::max(-1.0f, float(int16_t(words[i / 2] >> ((i & 1) * 16))) / 32767.0f) * 32.0f; break;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT: result[0] = std::bit_cast<float>(words[0]); break;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: result[0] = std::bit_cast<float>(words[0]); result[1] = std::bit_cast<float>(words[1]); break;
    default: throw std::runtime_error("resolve source conversion unsupported");
  }
  return result;
}
void PackColor(xenos::TextureFormat format, const std::array<float,4>& color, uint8_t* output) {
  auto unorm = [](float v, uint32_t max) { return uint32_t(std::nearbyint(std::clamp(v, 0.0f, 1.0f) * float(max))); };
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_8_8_8_8: {
      const uint32_t count = format == xenos::TextureFormat::k_8 ? 1 : format == xenos::TextureFormat::k_8_8 ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) output[i] = uint8_t(unorm(color[i], 255)); break;
    }
    case xenos::TextureFormat::k_2_10_10_10: {
      const uint32_t packed = unorm(color[0],1023) | (unorm(color[1],1023) << 10) | (unorm(color[2],1023) << 20) | (unorm(color[3],3) << 30);
      std::memcpy(output, &packed, 4); break;
    }
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: {
      const uint32_t count = format == xenos::TextureFormat::k_16_FLOAT ? 1 : format == xenos::TextureFormat::k_16_16_FLOAT ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) { const uint16_t v = rex::float_to_xenos_half(color[i], true); std::memcpy(output + i * 2, &v, 2); } break;
    }
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_16_16: {
      const uint32_t count = format == xenos::TextureFormat::k_16 ? 1 : format == xenos::TextureFormat::k_16_16 ? 2 : 4;
      for (uint32_t i = 0; i < count; ++i) { const uint16_t v = uint16_t(unorm(color[i], 65535)); std::memcpy(output + i * 2, &v, 2); } break;
    }
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: {
      const uint32_t count = format == xenos::TextureFormat::k_32_FLOAT ? 1 : format == xenos::TextureFormat::k_32_32_FLOAT ? 2 : 4;
      std::memcpy(output, color.data(), count * 4); break;
    }
    default: throw std::runtime_error("resolve destination conversion unsupported");
  }
}

}
MetalCommandProcessor::GuestRenderer::TransientSlice
MetalCommandProcessor::GuestRenderer::Transient(std::span<const std::byte> bytes, size_t alignment) {
  if (!transient_ring) transient_ring = host.CreateBufferSized(kTransientCapacity, "Xenos transient ring");
  const size_t offset = (transient_cursor + alignment - 1) & ~(alignment - 1);
  Need(offset + bytes.size() <= kTransientCapacity, "transient upload ring exhausted");
  host.UpdateBuffer(transient_ring, offset, bytes);
  transient_cursor = offset + bytes.size();
  return {transient_ring, offset};
}
MetalCommandProcessor::GuestRenderer::GuestRenderer(HostBackend& h, RegisterFile& r, rex::memory::Memory& m)
    : host(h), regs(r), memory(m), edram(2048 * 5120) {
  Translator::Features features(false);
  features.spirv_version = 0x00010300;
  features.max_storage_buffer_range = 512 * 1024 * 1024;
  features.full_draw_index_uint32 = true;
  features.clip_distance = true;
  // EDRAM ordering is provided by Metal raster-order groups. Use pixel
  // interlock in the shared translator, as the native Metal implementation
  // does; sample interlock selects a different SPIR-V lowering contract.
  features.fragment_shader_sample_interlock = false;
  features.sample_rate_shading = true;
  // Swizzling remains in the translated shader, including constant 0 and 1.
  features.image_view_format_swizzle = false;
  translator = std::make_unique<Translator>(features, false, false, true);
}
ShaderCode MetalCommandProcessor::GuestRenderer::Translate(SpirvShader& shader, Translator::Modification mod) {
  auto* translation = shader.GetOrCreateTranslation(mod.value);
  if (!translation->is_translated()) Need(translator->TranslateAnalyzedShader(*translation), "guest shader translation failed");
  Need(translation->is_valid(), "invalid guest shader translation");
  ShaderCode result; result.stage = shader.type() == xenos::ShaderType::kVertex ? Stage::kVertex : Stage::kFragment;
  result.flip_y = result.stage == Stage::kVertex;
  const auto& bytes = translation->translated_binary();
  Need(bytes.size() % 4 == 0, "unaligned translated shader");
  result.spirv.resize(bytes.size() / 4); std::memcpy(result.spirv.data(), bytes.data(), bytes.size());
  return result;
}
size_t MetalCommandProcessor::GuestRenderer::EdramAddress(uint32_t base, uint32_t pitch_tiles, uint32_t x, uint32_t y) const {
  // Xenos 32bpp tiles contain 80x16 samples and wrap after 2048 tiles.
  const uint32_t tile = (base + (y / 16) * pitch_tiles + x / 80) & 2047;
  return size_t(tile) * 5120 + ((y % 16) * 80 + x % 80) * 4;
}
void MetalCommandProcessor::GuestRenderer::FlushColor() {
  if (!edram_buffer) return;
  const auto bytes = host.ReadBuffer(edram_buffer);
  std::memcpy(edram.data(), bytes.data(), edram.size());
}
void MetalCommandProcessor::GuestRenderer::InvalidateTextureHashes(uint64_t start, uint64_t length) {
  if (!length) return;
  const uint64_t end = start + length;
  for (auto& [key, hash] : texture_hashes_by_descriptor) {
    const uint64_t base = (uint64_t(hash.base_page) << 12) & 0x1fffffff;
    const uint64_t mip = (uint64_t(hash.mip_page) << 12) & 0x1fffffff;
    if ((hash.base_bytes && base < end && start < base + hash.base_bytes) ||
        (hash.mip_bytes && mip < end && start < mip + hash.mip_bytes))
      hash.frame = UINT64_MAX;
  }
}
std::shared_ptr<Target> MetalCommandProcessor::GuestRenderer::Texture(uint32_t index, bool is_signed) {
  ScopeTimer timer(texture_us);
  ++texture_calls;
  const auto fetch = regs.GetTextureFetch(index);
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    // Shaders may declare a sampler whose fetch constant is not a texture
    // (unbound/default slot). Bind a transparent 1x1 array instead of failing.
    if (!null_texture) {
      std::vector<std::byte> zero(4);
      const std::array<TextureLevel, 1> level{{{1, 1, zero, 1}}};
      TargetDesc desc{};
      desc.width = 1; desc.height = 1; desc.format = Format::kRGBA8;
      desc.label = "Xenos null texture"; desc.array_texture = true; desc.mip_levels = 1;
      null_texture = host.CreateTexture2D(desc, level);
    }
    return null_texture;
  }
  Need(fetch.dimension == xenos::DataDimension::k1D ||
       fetch.dimension == xenos::DataDimension::k2DOrStacked ||
       fetch.dimension == xenos::DataDimension::k3D ||
       fetch.dimension == xenos::DataDimension::kCube, "guest texture dimension unsupported");
  const bool is_1d = fetch.dimension == xenos::DataDimension::k1D;
  const bool is_3d = fetch.dimension == xenos::DataDimension::k3D;
  const bool is_cube = fetch.dimension == xenos::DataDimension::kCube;
  Format host_format; const auto format = GetBaseFormat(fetch.format);
  switch (format) {
    case xenos::TextureFormat::k_8: host_format = Format::kR8; break;
    case xenos::TextureFormat::k_8_8: host_format = Format::kRG8; break;
    case xenos::TextureFormat::k_8_8_8_8: host_format = Format::kRGBA8; break;
    case xenos::TextureFormat::k_16: host_format = Format::kR16; break;
    case xenos::TextureFormat::k_16_16: host_format = Format::kRG16; break;
    case xenos::TextureFormat::k_16_16_16_16: host_format = Format::kRGBA16; break;
    case xenos::TextureFormat::k_16_FLOAT: host_format = Format::kR16Float; break;
    case xenos::TextureFormat::k_16_16_FLOAT: host_format = Format::kRG16Float; break;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: host_format = Format::kRGBA16Float; break;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT: host_format = Format::kR32Float; break;
    case xenos::TextureFormat::k_32_32_FLOAT: host_format = Format::kRG32Float; break;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: host_format = Format::kRGBA32Float; break;
    case xenos::TextureFormat::k_DXT1: host_format = Format::kBC1; break;
    case xenos::TextureFormat::k_DXT2_3: host_format = Format::kBC2; break;
    case xenos::TextureFormat::k_DXT4_5: host_format = Format::kBC3; break;
    case xenos::TextureFormat::k_DXN: host_format = Format::kBC5; break;
    default: throw std::runtime_error("guest texture format " + std::to_string(uint32_t(format)) + " unsupported");
  }
  if (is_signed) {
    switch (host_format) {
      case Format::kR8: host_format = Format::kR8Snorm; break;
      case Format::kRG8: host_format = Format::kRG8Snorm; break;
      case Format::kRGBA8: host_format = Format::kRGBA8Snorm; break;
      case Format::kR16: host_format = Format::kR16Snorm; break;
      case Format::kRG16: host_format = Format::kRG16Snorm; break;
      case Format::kRGBA16: host_format = Format::kRGBA16Snorm; break;
      case Format::kBC5: host_format = Format::kBC5Snorm; break;
      default: break;
    }
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page, &mip_page, &mip_min, &mip_max);
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  Need(base_page && mip_min == 0, "mip-only guest texture unsupported");
  const uint32_t depth = depth_minus_1 + 1;
  const uint32_t array_length = is_cube ? 6 :
      fetch.dimension == xenos::DataDimension::k2DOrStacked ? depth : 1;
  const auto layout = texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height,
      is_3d || fetch.stacked ? depth : 1, fetch.tiled, format, fetch.packed_mips, true, mip_max);
  if (fetch.stacked && array_length > 1) {
    static uint32_t stacked_logged = 0;
    if (stacked_logged++ < 4)
      std::fprintf(stderr, "[metal4-xenos] stacked texture %u has %u slices; uploading slice 0 only\n",
                   index, array_length);
  }
  // Guest writes can change textures without changing fetch constants, so the
  // guest bytes must be invalidated. Rehash only when the descriptor is new,
  // its range intersects a resolve destination, or the bounded rehash interval
  // elapsed; repeated draws of static textures reuse the cached hash.
  const auto lookup_begin = std::chrono::steady_clock::now();
  std::string descriptor_key(reinterpret_cast<const char*>(&fetch), sizeof(fetch));
  descriptor_key.push_back(is_signed ? 1 : 0);
  auto compute_hash = [&](uint32_t page, uint32_t bytes) -> uint64_t {
    if (!page || !bytes) return 0;
    const uint32_t base = (page << 12) & 0x1fffffff;
    Need(uint64_t(base) + bytes <= 512 * 1024 * 1024, "texture cache range exceeds physical memory");
    ++texture_hashes; texture_hash_bytes += bytes;
    return XXH3_64bits(memory.TranslatePhysical(base), bytes);
  };
  const uint32_t base_page_bytes = layout.base.level_data_extent_bytes;
  const uint32_t mip_page_bytes = layout.mips_total_extent_bytes;
  const uint64_t base_guest = (uint64_t(base_page) << 12) & 0x1fffffff;
  const uint64_t mip_guest = (uint64_t(mip_page) << 12) & 0x1fffffff;
  auto overlaps = [&](const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                      uint64_t start, uint32_t bytes) {
    if (!bytes) return false;
    const uint64_t end = start + bytes;
    for (const auto& range : ranges)
      if (start < range.second && range.first < end) return true;
    return false;
  };
  const bool dirty = overlaps(texture_dirty_prev, base_guest, base_page_bytes) ||
      overlaps(texture_dirty_prev, mip_guest, mip_page_bytes) ||
      overlaps(texture_dirty_current, base_guest, base_page_bytes) ||
      overlaps(texture_dirty_current, mip_guest, mip_page_bytes);
  auto& cached_hash = texture_hashes_by_descriptor[descriptor_key];
  const bool changed_layout = cached_hash.base_page != base_page ||
      cached_hash.mip_page != mip_page || cached_hash.base_bytes != base_page_bytes ||
      cached_hash.mip_bytes != mip_page_bytes;
  const bool force_hash = index == 0 &&
      std::getenv("MGR_METAL4_FRONTBUFFER_REHASH") != nullptr;
  const bool rehash = force_hash || changed_layout || cached_hash.frame == UINT64_MAX ||
      (dirty && cached_hash.frame != guest_frame) ||
      (cached_hash.frame != UINT64_MAX &&
       guest_frame - cached_hash.frame >= kTextureHashInterval);
  if (rehash) {
    cached_hash.base_hash = compute_hash(base_page, base_page_bytes);
    cached_hash.mip_hash = compute_hash(mip_page, mip_page_bytes);
    cached_hash.frame = guest_frame;
    cached_hash.base_page = base_page; cached_hash.mip_page = mip_page;
    cached_hash.base_bytes = base_page_bytes; cached_hash.mip_bytes = mip_page_bytes;
  }
  std::string cache_key = descriptor_key;
  cache_key.append(reinterpret_cast<const char*>(&cached_hash.base_hash), sizeof(uint64_t));
  cache_key.append(reinterpret_cast<const char*>(&cached_hash.mip_hash), sizeof(uint64_t));
  texture_lookup_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - lookup_begin).count());
  ++texture_clock;
  if (auto existing = texture_cache.find(cache_key); existing != texture_cache.end()) {
    ++texture_cache_hits;
    existing->second.used = texture_clock; return existing->second.image;
  }
  ++texture_uploads;
  ++texture_cache_misses;
  if (unique_texture_keys.insert(descriptor_key).second) ++texture_unique;
  const auto& info = *FormatInfo::Get(format);
  const uint32_t block_width = info.block_width, block_height = info.block_height;
  const uint32_t block_bytes = info.bits_per_pixel * block_width * block_height / 8;
  Need(block_bytes && std::has_single_bit(block_bytes), "invalid texture block size");
  const uint32_t block_log2 = std::countr_zero(block_bytes);
  std::vector<TextureLevel> levels;
  const auto translate_begin = std::chrono::steady_clock::now();
  for (uint32_t mip = 0; mip <= mip_max; ++mip) {
    TextureLevel level{};
    level.width = std::max(1u, width >> mip);
    level.height = std::max(1u, height >> mip);
    level.depth = is_3d ? std::max(1u, depth >> mip) : (fetch.stacked || is_cube ? array_length : 1);
    const auto& storage = mip ? layout.mips[std::min(mip, layout.packed_level)] : layout.base;
    const uint32_t blocks_x = (level.width + block_width - 1) / block_width;
    const uint32_t blocks_y = (level.height + block_height - 1) / block_height;
    const uint32_t pitch_blocks = storage.row_pitch_bytes / block_bytes;
    const uint32_t slice_extent = storage.array_slice_data_extent_bytes;
    uint32_t ox = 0, oy = 0, oz = 0;
    if (fetch.packed_mips) texture_util::GetPackedMipOffset(width, height, is_3d ? depth : 1, format, mip, ox, oy, oz);
    const uint32_t base = ((mip ? mip_page : base_page) << 12) & 0x1fffffff;
    const uint32_t level_offset = mip ? layout.mip_offsets_bytes[mip] : 0;
    level.bytes.resize(size_t(blocks_x) * blocks_y * level.depth * block_bytes);
    for (uint32_t slice = 0; slice < level.depth; ++slice) {
      const uint32_t z = is_3d ? slice : 0;
      const uint32_t slice_offset = is_3d ? 0 : slice * slice_extent;
      for (uint32_t y = 0; y < blocks_y; ++y) for (uint32_t x = 0; x < blocks_x; ++x) {
        uint32_t offset;
        if (fetch.tiled && is_3d) {
          offset = uint32_t(TiledOffset3D(int32_t(x + ox), int32_t(y + oy), int32_t(z + oz),
              pitch_blocks, storage.y_extent_blocks, block_log2));
        } else if (fetch.tiled) {
          offset = uint32_t(TiledOffset2D(int32_t(x + ox), int32_t(y + oy),
              pitch_blocks, block_log2)) + slice_offset;
        } else {
          offset = slice_offset + (y + oy) * storage.row_pitch_bytes + (x + ox) * block_bytes;
        }
        const uint64_t address = uint64_t(base) + level_offset + offset;
        Need(address + block_bytes <= 512 * 1024 * 1024, "guest texture exceeds physical memory");
        auto* output = level.bytes.data() + ((size_t(slice) * blocks_y + y) * blocks_x + x) * block_bytes;
        // Blocks are contiguous in guest physical memory. Translate once and
        // apply the format's lane permutation, rather than translating every
        // byte (this is the texture-upload hot loop).
        const auto* source_bytes = memory.TranslatePhysical(uint32_t(address));
        // Apply the fetch endianness within each 32-bit word, including BC
        // blocks, as the Vulkan texture-load shader does.
        const bool block_compressed = format == xenos::TextureFormat::k_DXT1 ||
            format == xenos::TextureFormat::k_DXT2_3 ||
            format == xenos::TextureFormat::k_DXT4_5 ||
            format == xenos::TextureFormat::k_DXN;
        mgr::metal4::CopyGuestTextureBlock(block_compressed, uint32_t(fetch.endianness),
            source_bytes, reinterpret_cast<uint8_t*>(output), block_bytes);
        if (format == xenos::TextureFormat::k_24_8 || format == xenos::TextureFormat::k_24_8_FLOAT) {
          const auto convert_begin = std::chrono::steady_clock::now();
          uint32_t packed; std::memcpy(&packed, output, 4);
          const float depth_value = format == xenos::TextureFormat::k_24_8_FLOAT ? xenos::Float20e4To32(packed >> 8) : float(packed >> 8) / 16777215.0f;
          std::memcpy(output, &depth_value, 4);
          texture_convert_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - convert_begin).count());
        }
      }
    }
    levels.push_back(std::move(level));
  }
  texture_translate_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - translate_begin).count());
  for (const auto& level : levels) texture_bytes += level.bytes.size();
  TargetDesc desc{};
  desc.width = width; desc.height = height; desc.depth = is_3d ? depth : 1;
  desc.format = host_format; desc.label = "Xenos texture " + std::to_string(index);
  desc.array_texture = !is_1d && !is_3d; desc.array_length = array_length;
  desc.mip_levels = mip_max + 1; desc.is_3d = is_3d; desc.is_1d = is_1d; desc.is_cube = is_cube;
  const auto upload_begin = std::chrono::steady_clock::now();
  auto image = host.CreateTexture2D(desc, levels);
  texture_upload_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - upload_begin).count());
  if (std::getenv("MGR_METAL4_TRACE_EDRAM") && format == xenos::TextureFormat::k_8_8_8_8) {
    static uint32_t texture_readbacks = 0;
    if (texture_readbacks++ < 4) {
      const uint32_t source_nonzero = levels.empty() ? 0 : CountNonzeroDwords(
          reinterpret_cast<const uint8_t*>(levels[0].bytes.data()), levels[0].bytes.size());
      uint32_t source_rgb = 0, source_alpha = 0;
      if (!levels.empty()) {
        const auto& source = levels[0].bytes;
        for (size_t i = 0; i + 4 <= source.size(); i += 4) {
          source_rgb += (uint8_t(source[i]) | uint8_t(source[i + 1]) |
                         uint8_t(source[i + 2])) != 0;
          source_alpha += uint8_t(source[i + 3]) != 0;
        }
      }
      auto pixels = host.Readback(image);
      uint32_t black = 0;
      for (size_t i = 0; i + 4 <= pixels.size(); i += 4)
        black += (pixels[i] | pixels[i + 1] | pixels[i + 2]) == 0;
      std::fprintf(stderr,
          "[metal4-xenos] texture fetch=%u base=%08x format=%u %ux%u mips=%u endian=%u swizzle=%03x "
          "source_nonzero=%u source_rgb=%u source_alpha=%u first=%02x%02x%02x%02x "
          "readback_black=%u/%u\n",
          index, uint32_t(fetch.base_address) << 12, uint32_t(format), width, height, mip_max + 1,
          uint32_t(fetch.endianness), uint32_t(fetch.swizzle), source_nonzero, source_rgb,
          source_alpha, !levels.empty() && levels[0].bytes.size() >= 4 ? uint8_t(levels[0].bytes[0]) : 0,
          !levels.empty() && levels[0].bytes.size() >= 4 ? uint8_t(levels[0].bytes[1]) : 0,
          !levels.empty() && levels[0].bytes.size() >= 4 ? uint8_t(levels[0].bytes[2]) : 0,
          !levels.empty() && levels[0].bytes.size() >= 4 ? uint8_t(levels[0].bytes[3]) : 0,
          black, width * height);
    }
  }
  size_t cost = 0; for (const auto& level : levels) cost += level.bytes.size();
  constexpr size_t limit = 256 * 1024 * 1024;
  while (!texture_cache.empty() && texture_cache_bytes + cost > limit) {
    auto oldest = std::min_element(texture_cache.begin(), texture_cache.end(), [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
    texture_cache_bytes -= oldest->second.bytes; texture_cache.erase(oldest);
  }
  if (cost <= limit) { texture_cache.emplace(std::move(cache_key), CachedTexture{image, cost, texture_clock}); texture_cache_bytes += cost; }
  return image;
}
void MetalCommandProcessor::GuestRenderer::Draw(xenos::PrimitiveType primitive, uint32_t count,
    const IndexBufferInfo* indices, SpirvShader* vs, SpirvShader* ps) {
  ScopeTimer timer(draw_us);
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  static uint64_t draw_number = 0;
  ++draw_number;
  const bool trace_draw = std::getenv("MGR_METAL4_TRACE_EDRAM") != nullptr;
  if (trace_draw) {
    const auto ci = regs.Get<reg::RB_COLOR_INFO>();
    const auto depth_info_log = regs.Get<reg::RB_DEPTH_INFO>();
    const auto ps_log = static_cast<SpirvShader*>(ps);
    const uint32_t rb_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t normalized_mask = ps_log ? draw_util::GetNormalizedColorMask(
        regs, ps_log->writes_color_targets()) : 0;
    static uint64_t logged_color_draws = 0;
    if (draw_number <= 12 || (normalized_mask && logged_color_draws++ < 24) ||
        draw_number % 500 == 0) {
      std::fprintf(stderr,
          "[metal4-xenos] draw #%llu prim=%u count=%u mode=%u color_base=%u color_fmt=%u "
          "rb_mask=%04x norm_mask=%04x ps_writes=%08x pitch=%u msaa=%u depth_base=%u z_en=%u\n",
          static_cast<unsigned long long>(draw_number), uint32_t(primitive), count, uint32_t(mode),
          ci.color_base, uint32_t(ci.color_format), rb_mask, normalized_mask,
          ps_log ? ps_log->writes_color_targets() : 0,
          surface.surface_pitch, uint32_t(surface.msaa_samples), depth_info_log.depth_base,
          uint32_t(regs.Get<reg::RB_DEPTHCONTROL>().z_enable));
    }
  }
  if (mode == xenos::EdramMode::kCopy) { Resolve(); return; }
  Need(vs, "Xenos draw missing vertex shader");
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!surface.surface_pitch || !draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) {
    Need(!vs->memexport_eM_written(), "vertex memexport unsupported"); return;
  }
  Need(!vs->memexport_eM_written() && (!ps || !ps->memexport_eM_written()), "guest memexport unsupported");
  if (mode != xenos::EdramMode::kColorDepth) ps = nullptr;
  const auto depth = draw_util::GetNormalizedDepthControl(regs);

  const auto color_info = regs.Get<reg::RB_COLOR_INFO>();
  const auto mask = ps ? draw_util::GetNormalizedColorMask(regs, ps->writes_color_targets()) : 0;
  const bool trace_scene = std::getenv("MGR_METAL4_TRACE_SCENE") &&
      frontbuffer_present + 1 >= EnvU32("MGR_METAL4_TRACE_FROM", 113) &&
      frontbuffer_present + 1 <= EnvU32("MGR_METAL4_TRACE_TO", 114);
  if (trace_scene && mask) {
    std::fprintf(stderr,
        "[metal4-pm4] draw=%llu present=%u prim=%u count=%u ps=%016llx writes=%x "
        "rb_mask=%04x norm_mask=%04x depth=%08x depth_base=%u pitch=%u msaa=%u "
        "rt0=%u/%u rt1=%u/%u rt2=%u/%u rt3=%u/%u "
        "cull_front=%u cull_back=%u guest_face=%u host_clockwise=%u\n",
        static_cast<unsigned long long>(draw_number), frontbuffer_present + 1,
        uint32_t(primitive), count, static_cast<unsigned long long>(ps->ucode_data_hash()),
        ps->writes_color_targets(), regs[XE_GPU_REG_RB_COLOR_MASK], mask,
        regs[XE_GPU_REG_RB_DEPTHCONTROL], regs.Get<reg::RB_DEPTH_INFO>().depth_base,
        surface.surface_pitch, uint32_t(surface.msaa_samples),
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_base,
        uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_format),
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[1]).color_base,
        uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[1]).color_format),
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[2]).color_base,
        uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[2]).color_format),
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[3]).color_base,
        uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[3]).color_format),
        uint32_t(regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front),
        uint32_t(regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_back),
        uint32_t(regs.Get<reg::PA_SU_SC_MODE_CNTL>().face),
        uint32_t(!regs.Get<reg::PA_SU_SC_MODE_CNTL>().face));
  }
  draw_util::Scissor scissor; draw_util::GetScissor(regs, scissor);
  if (!scissor.extent[0] || !scissor.extent[1]) return;
  // Height is derived from the guest scissor, not an assumed game resolution.
  const uint32_t width = surface.surface_pitch, height = scissor.offset[1] + scissor.extent[1];
  Need(width <= 8192 && height <= 8192, "guest render target extent exceeds limits");
  
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, 1, 1, false, width, height, true, depth, false, false, ps && ps->writes_depth(), viewport);
  if (!viewport.xy_extent[0] || !viewport.xy_extent[1]) return;
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  Need(clip.clip_disable || !clip.ucp_cull_only_ena || !clip.ucp_ena, "guest cull-only user clipping unsupported");
  Need(regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select != xenos::VGTOutputPath::kTessellationEnable,
      "guest tessellation unsupported");
  const bool expanded = primitive == xenos::PrimitiveType::kRectangleList || primitive == xenos::PrimitiveType::kPointList;
  auto program = regs.Get<reg::SQ_PROGRAM_CNTL>(); uint32_t param_gen = UINT32_MAX;
  const uint32_t interpolators = ps ? vs->writes_interpolators() & ps->GetInterpolatorInputMask(program, regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen) : 0;
  Translator::Modification vm(translator->GetDefaultVertexShaderModification(vs->GetDynamicAddressableRegisterCount(program.vs_num_reg),
      primitive == xenos::PrimitiveType::kRectangleList ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip : primitive == xenos::PrimitiveType::kPointList ? Shader::HostVertexShaderType::kPointListAsTriangleStrip : Shader::HostVertexShaderType::kVertex));
  vm.vertex.interpolator_mask = interpolators;
  vm.vertex.user_clip_plane_count = clip.clip_disable ? 0 : std::popcount(uint32_t(clip.ucp_ena));
  vm.vertex.point_ps_ucp_mode = clip.ps_ucp_mode;
  Need(!(vs->writes_point_size_edge_flag_kill_vertex() & 4) || clip.vtx_kill_or, "guest AND vertex killing unsupported");
  vm.vertex.output_point_parameters = primitive == xenos::PrimitiveType::kPointList && param_gen < xenos::kMaxInterpolators;
  Translator::Modification pm(translator->GetDefaultPixelShaderModification(ps ? ps->GetDynamicAddressableRegisterCount(program.ps_num_reg) : 0));
  pm.pixel.interpolator_mask = interpolators;
  pm.pixel.interpolators_centroid = interpolators & ~xenos::GetInterpolatorSamplingPattern(surface.msaa_samples, regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl, regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen < xenos::kMaxInterpolators) { pm.pixel.param_gen_enable = 1; pm.pixel.param_gen_interpolator = param_gen; pm.pixel.param_gen_point = primitive == xenos::PrimitiveType::kPointList; }
  PipelineDesc pipeline; pipeline.vertex = Translate(*vs, vm);
  if (ps) pipeline.fragment = Translate(*ps, pm);
  else {
    const auto binary = translator->CreateDepthOnlyFragmentShader();
    pipeline.fragment.stage = Stage::kFragment;
    pipeline.fragment.spirv.resize(binary.size() / 4);
    std::memcpy(pipeline.fragment.spirv.data(), binary.data(), binary.size());
  }
  pipeline.color = Format::kNone;
  pipeline.raster_samples = surface.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 4;
  pipeline.color_write_mask = 0;
  pipeline.label = "Xenos ROG " + std::to_string(vs->ucode_data_hash()) + "/" + std::to_string(ps ? ps->ucode_data_hash() : 0);
  DrawDesc draw; draw.count = count;
  switch (primitive) {
    case xenos::PrimitiveType::kPointList:
    case xenos::PrimitiveType::kRectangleList:
      draw.primitive = Primitive::kTriangles; break;
    case xenos::PrimitiveType::kTriangleList: draw.primitive = Primitive::kTriangles; break;
    case xenos::PrimitiveType::kTriangleFan: draw.primitive = Primitive::kTriangles; break;
    case xenos::PrimitiveType::kTriangleStrip: draw.primitive = Primitive::kTriangleStrip; break;
    case xenos::PrimitiveType::kLineList: draw.primitive = Primitive::kLines; break;
    case xenos::PrimitiveType::kLineStrip: draw.primitive = Primitive::kLineStrip; break;
    default: throw std::runtime_error("guest primitive " + std::to_string(uint32_t(primitive)) + " unsupported");
  }
  // A correctness-first upload path: GPU readers finish before shared bytes
  // are overwritten. Replace with a transient buffer ring after parity.
  host.WaitIdle();
  transient_cursor = 0;
  if (!edram_buffer) edram_buffer = host.CreateBuffer(std::as_bytes(std::span(edram)), "Xenos 10 MiB EDRAM");
  if (!shared_memory) shared_memory = host.CreateBufferSized(512 * 1024 * 1024, "Xenos physical memory");
  for (const auto& binding : vs->vertex_bindings()) {
    const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
    Need(fetch.type == xenos::FetchConstantType::kVertex, "invalid vertex fetch constant");
    const uint32_t address = (fetch.address << 2) & 0x1fffffff, size = fetch.size * 4;
    Need(size && uint64_t(address) + size <= 512 * 1024 * 1024, "vertex fetch range exceeds physical memory");
    host.UpdateBuffer(shared_memory, address, {reinterpret_cast<const std::byte*>(memory.TranslatePhysical(address)), size});
  }
  const bool primitive_restart = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
      indices && (primitive == xenos::PrimitiveType::kTriangleStrip ||
                  primitive == xenos::PrimitiveType::kLineStrip);
  std::vector<uint32_t> fan_vertices;
  if (indices) {
    Need(indices->guest_base && indices->length && indices->count >= count, "invalid guest index buffer");
    const uint32_t size = indices->format == xenos::IndexFormat::kInt32 ? 4 : 2;
    Need(uint64_t(count) * size <= indices->length, "guest index buffer truncated");
    std::vector<uint8_t> swapped(size_t(count) * size);
    const auto* source = memory.TranslatePhysical(indices->guest_base & 0x1fffffff);
    for (uint32_t i = 0; i < count; ++i) {
      if (size == 4) { uint32_t v; std::memcpy(&v, source + size_t(i) * 4, 4); v = xenos::GpuSwap(v, indices->endianness) & 0xffffff; std::memcpy(swapped.data() + size_t(i) * 4, &v, 4); }
      else { uint16_t v; std::memcpy(&v, source + size_t(i) * 2, 2); v = xenos::GpuSwap(v, indices->endianness); std::memcpy(swapped.data() + size_t(i) * 2, &v, 2); }
    }
    if (primitive == xenos::PrimitiveType::kTriangleFan) {
      fan_vertices.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t value;
        if (size == 4) std::memcpy(&value, swapped.data() + size_t(i) * 4, 4);
        else { uint16_t short_value; std::memcpy(&short_value, swapped.data() + size_t(i) * 2, 2); value = short_value; }
        fan_vertices.push_back(value);
      }
    }
    if (expanded) {
      const uint32_t address = indices->guest_base & 0x1fffffff;
      host.UpdateBuffer(shared_memory, address, {reinterpret_cast<const std::byte*>(source), size_t(count) * size});
    } else if (primitive_restart) {
      // Metal has no primitive restart for strips. Split at the restart index
      // and emit an explicit list, preserving the strip's alternating winding.
      const uint32_t restart_index = size == 4 ? 0xFFFFFFFFu : 0xFFFFu;
      std::vector<uint32_t> list;
      uint32_t segment = 0;
      for (uint32_t i = 0; i <= count; ++i) {
        uint32_t value = restart_index;
        if (i < count) {
          if (size == 4) std::memcpy(&value, swapped.data() + size_t(i) * 4, 4);
          else { uint16_t v; std::memcpy(&v, swapped.data() + size_t(i) * 2, 2); value = v; }
        }
        if (value != restart_index) continue;
        if (primitive == xenos::PrimitiveType::kTriangleStrip) {
          for (uint32_t t = segment; t + 2 < i; ++t) {
            if ((t - segment) & 1) { list.push_back(t + 1); list.push_back(t); }
            else { list.push_back(t); list.push_back(t + 1); }
            list.push_back(t + 2);
          }
        } else {
          for (uint32_t v = segment; v + 1 < i; ++v) { list.push_back(v); list.push_back(v + 1); }
        }
        segment = i + 1;
      }
      if (list.empty()) return;
      const auto slice = Transient(std::as_bytes(std::span(list)));
      draw.indices = slice.buffer; draw.first = uint32_t(slice.offset / 4);
      draw.indices32 = true; draw.count = uint32_t(list.size());
      draw.primitive = primitive == xenos::PrimitiveType::kTriangleStrip ? Primitive::kTriangles : Primitive::kLines;
    } else {
      const auto slice = Transient(std::as_bytes(std::span(swapped)));
      draw.indices = slice.buffer; draw.first = uint32_t(slice.offset / size);
      draw.indices32 = size == 4;
    }
  }
  if (primitive == xenos::PrimitiveType::kTriangleFan) {
    if (!indices) {
      fan_vertices.resize(count);
      for (uint32_t i = 0; i < count; ++i) fan_vertices[i] = i;
    }
    auto triangles = mgr::metal4::TriangleFanIndices(fan_vertices);
    if (triangles.empty()) return;
    const auto slice = Transient(std::as_bytes(std::span(triangles)));
    draw.indices = slice.buffer;
    draw.first = uint32_t(slice.offset / 4);
    draw.indices32 = true;
    draw.count = uint32_t(triangles.size());
  }
  Translator::SystemConstants constants{};
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vtx_xy_fmt) constants.flags |= Translator::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt) constants.flags |= Translator::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt) constants.flags |= Translator::kSysFlag_WNotReciprocal;
  if (polygonal) constants.flags |= Translator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) constants.flags |= Translator::kSysFlag_PrimitiveLine;
  const auto control = regs.Get<reg::RB_COLORCONTROL>();
  constants.flags |= uint32_t(control.alpha_test_enable ? control.alpha_func : xenos::CompareFunction::kAlways) << Translator::kSysFlag_AlphaPassIfLess_Shift;
  constants.alpha_to_mask = control.alpha_to_mask_enable ? (control.value >> 24) | (1u << 8) : 0;
  if (indices && expanded) {
    constants.flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (indices->format == xenos::IndexFormat::kInt32) constants.flags |= Translator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    constants.vertex_index_load_address = indices->guest_base & 0x1fffffff;
    constants.vertex_index_endian = indices->endianness;
  }
  uint32_t plane = 0;
  if (!clip.clip_disable) for (uint32_t i = 0; i < 6; ++i) if (clip.ucp_ena & (1u << i))
    std::memcpy(constants.user_clip_planes[plane++], &regs[XE_GPU_REG_PA_CL_UCP_0_X + i * 4], 16);
  constants.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  constants.vertex_index_min = regs[XE_GPU_REG_VGT_MIN_VTX_INDX]; constants.vertex_index_max = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
  std::copy_n(viewport.ndc_scale, 3, constants.ndc_scale); std::copy_n(viewport.ndc_offset, 3, constants.ndc_offset);
  constants.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  for (uint32_t i = 0; i < 4; ++i) constants.color_exp_bias[i] = std::ldexp(1.0f, regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_exp_bias);
  if (primitive == xenos::PrimitiveType::kPointList) {
    const auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>(); const auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    constants.point_vertex_diameter_min = minmax.min_size * (2.0f / 16.0f);
    constants.point_vertex_diameter_max = minmax.max_size * (2.0f / 16.0f);
    constants.point_constant_diameter[0] = size.width * (2.0f / 16.0f);
    constants.point_constant_diameter[1] = size.height * (2.0f / 16.0f);
    constants.point_screen_diameter_to_ndc_radius[0] = 1.0f / viewport.xy_extent[0];
    constants.point_screen_diameter_to_ndc_radius[1] = 1.0f / viewport.xy_extent[1];
  }
  if (expanded) {
    const uint32_t primitives = primitive == xenos::PrimitiveType::kRectangleList ? count / 3 : count;
    std::vector<uint32_t> adapter; adapter.reserve(size_t(primitives) * 6);
    for (uint32_t i = 0; i < primitives; ++i) { const uint32_t v = i * 4; adapter.insert(adapter.end(), {v, v+1, v+2, v+2, v+1, v+3}); }
    if (adapter.empty()) return;
    const auto slice = Transient(std::as_bytes(std::span(adapter)));
    draw.indices = slice.buffer; draw.first = uint32_t(slice.offset / 4);
    draw.indices32 = true; draw.count = uint32_t(adapter.size());
  }
  const auto used_textures = vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  const uint64_t ps_hash = ps ? ps->ucode_data_hash() : 0;
  const bool composite_ps = ps && (ps_hash == 0xf854bf3b7bf7aa9dull ||
                                   ps_hash == 0x4dfdc5afb71c5f74ull);
  const bool count_fragments = composite_ps && std::getenv("MGR_METAL4_FRAGMENT_COUNTERS");
  if (composite_ps && std::getenv("MGR_METAL4_DUMP_UCODE")) {
    const auto path = std::string("/private/tmp/mgr-") + std::to_string(ps_hash) + ".ucode";
    if (!std::filesystem::exists(path)) {
      std::ofstream output(path, std::ios::binary);
      const auto& code = ps->ucode_data();
      output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(uint32_t));
    }
  }
  const bool trace_composite = composite_ps && std::getenv("MGR_METAL4_TRACE_COMPOSITE") != nullptr;
  const bool dump_composite = composite_ps && std::getenv("MGR_METAL4_DUMP_COMPOSITE") != nullptr;
  const char* skip_mode = std::getenv("MGR_METAL4_SKIP_COMPOSITE");
  const bool skip_composite = composite_ps && skip_mode != nullptr;
  const bool skip_this = skip_composite && (
      !std::strcmp(skip_mode, "first") ? ps_hash == 0xf854bf3b7bf7aa9dull :
      !std::strcmp(skip_mode, "second") ? ps_hash == 0x4dfdc5afb71c5f74ull : true);
  std::map<uint32_t, std::shared_ptr<Target>> textures;
  for (uint32_t i = 0; i < 32; ++i) if (used_textures & (1u << i)) {
    const auto fetch = regs.GetTextureFetch(i); textures[i * 2] = Texture(i);
    if (trace_scene) {
      std::fprintf(stderr,
          "[metal4-pm4] draw=%llu fetch=%u format=%u base=%08x size=%ux%u "
          "swizzle=%03x signs=%02x\n",
          static_cast<unsigned long long>(draw_number), i,
          uint32_t(GetBaseFormat(fetch.format)), uint32_t(fetch.base_address) << 12,
          fetch.size_2d.width + 1, fetch.size_2d.height + 1,
          uint32_t(fetch.swizzle), uint32_t(texture_util::SwizzleSigns(fetch)));
    }
    const bool any_signed = fetch.sign_x == xenos::TextureSign::kSigned || fetch.sign_y == xenos::TextureSign::kSigned || fetch.sign_z == xenos::TextureSign::kSigned || fetch.sign_w == xenos::TextureSign::kSigned;
    textures[i * 2 + 1] = any_signed ? Texture(i, true) : textures[i * 2];
    constants.texture_swizzled_signs[i >> 2] |= uint32_t(texture_util::SwizzleSigns(fetch)) << ((i & 3) * 8);
    constants.texture_swizzles[i >> 1] |=
        mgr::metal4::ShaderTextureSwizzle(GetBaseFormat(fetch.format), fetch.swizzle)
        << ((i & 1) * 12);
  }
  // Preserve the shared shader output-merger contract byte for byte. Metal
  // raster-order groups serialize overlapping fragments accessing this buffer.
  constants.flags |= uint32_t(surface.msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
  auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  if (depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) constants.flags |= Translator::kSysFlag_DepthFloat24;
  if (depth.z_enable || depth.stencil_enable) {
    constants.flags |= Translator::kSysFlag_FSIDepthStencil;
    if (depth.z_enable) {
      constants.flags |= uint32_t(depth.zfunc) << Translator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (depth.z_write_enable) constants.flags |= Translator::kSysFlag_FSIDepthWrite;
    }
    if (depth.stencil_enable) constants.flags |= Translator::kSysFlag_FSIStencilTest;
    if ((!ps || !control.alpha_test_enable || control.alpha_func == xenos::CompareFunction::kAlways) && !control.alpha_to_mask_enable)
      constants.flags |= Translator::kSysFlag_FSIDepthStencilEarlyWrite;
  }
  constants.edram_32bpp_tile_pitch_dwords_scaled = xenos::GetSurfacePitchTiles(width, surface.msaa_samples, false) * 1280;
  constants.edram_depth_base_dwords_scaled = depth_info.depth_base * 1280;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    auto* clamp = constants.edram_rt_clamp[i];
    RenderTargetCache::GetPSIColorFormatInfo(info.color_format, (mask >> (i * 4)) & 15,
        clamp[0], clamp[1], clamp[2], clamp[3], constants.edram_rt_keep_mask[i][0], constants.edram_rt_keep_mask[i][1]);
    constants.edram_rt_base_dwords_scaled[i] = info.color_base * 1280;
    constants.edram_rt_format_flags[i] = RenderTargetCache::AddPSIColorFormatFlags(info.color_format);
    constants.edram_rt_blend_factors_ops[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1fff1fff;
    if (trace_composite) {
      std::fprintf(stderr,
          "[metal4-composite] rt logical=%u physical=buffer/none writes=%u rb_mask=%x norm_mask=%x "
          "base=%u format=%u keep=%08x,%08x blend=%08x edram_dwords=%u "
          "host_attachment_format=none host_write_mask=0 host_blend=disabled\n",
          i, bool(ps->writes_color_targets() & (1u << i)),
          (regs[XE_GPU_REG_RB_COLOR_MASK] >> (i * 4)) & 15,
          (mask >> (i * 4)) & 15, info.color_base, uint32_t(info.color_format),
          constants.edram_rt_keep_mask[i][0], constants.edram_rt_keep_mask[i][1],
          constants.edram_rt_blend_factors_ops[i], constants.edram_rt_base_dwords_scaled[i]);
    }
  }
  const auto rasterizer = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (polygonal ? rasterizer.poly_offset_front_enable : rasterizer.poly_offset_para_enable) {
    constants.edram_poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) * xenos::kPolygonOffsetScaleSubpixelUnit;
    constants.edram_poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  if (polygonal && rasterizer.poly_offset_back_enable) {
    constants.edram_poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE) * xenos::kPolygonOffsetScaleSubpixelUnit;
    constants.edram_poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
  } else if (!polygonal) {
    constants.edram_poly_offset_back_scale = constants.edram_poly_offset_front_scale;
    constants.edram_poly_offset_back_offset = constants.edram_poly_offset_front_offset;
  }
  constants.edram_stencil_front_reference_masks = regs[XE_GPU_REG_RB_STENCILREFMASK] & 0xffffff;
  constants.edram_stencil_front_func_ops = (depth.value >> 8) & 0xfff;
  constants.edram_stencil_back_reference_masks = polygonal && depth.backface_enable ? regs[XE_GPU_REG_RB_STENCILREFMASK_BF] & 0xffffff : constants.edram_stencil_front_reference_masks;
  constants.edram_stencil_back_func_ops = polygonal && depth.backface_enable ? (depth.value >> 20) & 0xfff : constants.edram_stencil_front_func_ops;
  constants.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  constants.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  constants.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  constants.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  const auto system = Transient(Bytes(constants));
  const auto bool_loop = Transient(std::as_bytes(std::span(&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40)));
  const auto fetches = Transient(std::as_bytes(std::span(&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192)));
  auto bind = [&](SpirvShader& shader, Stage stage) {
    const auto& map = shader.constant_register_map(); std::vector<uint32_t> floats;
    const uint32_t base = stage == Stage::kVertex ? XE_GPU_REG_SHADER_CONSTANT_000_X : XE_GPU_REG_SHADER_CONSTANT_256_X;
    for (uint32_t word = 0; word < 4; ++word) for (uint32_t bit = 0; bit < 64; ++bit)
      if (map.float_bitmap[word] & (uint64_t(1) << bit)) {
        const auto* p = &regs[base + (word * 64 + bit) * 4]; floats.insert(floats.end(), p, p + 4);
      }
    if (floats.empty()) floats.resize(4);
    const auto packed = Transient(std::as_bytes(std::span(floats)));
    if (trace_composite && stage == Stage::kFragment) {
      std::fprintf(stderr,
          "[metal4-composite] constants draw=%llu system_offset=%zu float_offset=%zu "
          "float_regs=%zu first=%08x,%08x,%08x,%08x flags=%08x alpha_to_mask=%08x\n",
          static_cast<unsigned long long>(draw_number), system.offset, packed.offset,
          floats.size() / 4, floats[0], floats[1], floats[2], floats[3],
          constants.flags, constants.alpha_to_mask);
    }
    draw.buffers.push_back({stage, 0, 0, shared_memory});
    draw.buffers.push_back({stage, 0, 1, edram_buffer});
    draw.buffers.push_back({stage, 1, Translator::kConstantBufferSystem, system.buffer, system.offset});
    draw.buffers.push_back({stage, 1, stage == Stage::kVertex ? Translator::kConstantBufferFloatVertex : Translator::kConstantBufferFloatPixel, packed.buffer, packed.offset});
    draw.buffers.push_back({stage, 1, Translator::kConstantBufferBoolLoop, bool_loop.buffer, bool_loop.offset});
    draw.buffers.push_back({stage, 1, Translator::kConstantBufferFetch, fetches.buffer, fetches.offset});
    uint32_t slot = 0; const uint32_t set = stage == Stage::kVertex ? 2 : 3;
    for (const auto& binding : shader.GetTextureBindingsAfterTranslation()) {
      Need(binding.dimension == xenos::FetchOpDimension::k2D ||
           binding.dimension == xenos::FetchOpDimension::k1D ||
           binding.dimension == xenos::FetchOpDimension::k3DOrStacked ||
           binding.dimension == xenos::FetchOpDimension::kCube,
           "guest shader texture dimension unsupported");
      auto bound = textures.at(binding.fetch_constant * 2 + binding.is_signed);
      const char* override_fetch = std::getenv("MGR_METAL4_OVERRIDE_FETCH");
      if (stage == Stage::kFragment && ps_hash == 0xf854bf3b7bf7aa9dull &&
          override_fetch && *override_fetch &&
          std::strtoul(override_fetch, nullptr, 10) == binding.fetch_constant) {
        if (!diagnostic_texture) {
          const std::array<std::byte, 4> gray{
              std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255}};
          diagnostic_texture = host.CreateTextureRGBA8(1, 1, gray, "diagnostic fetch", true);
        }
        bound = diagnostic_texture;
      }
      if (trace_composite && stage == Stage::kFragment) {
        const auto dfetch = regs.GetTextureFetch(binding.fetch_constant);
        uint32_t w1 = 0, h1 = 0, d1 = 0, bpage = 0, mpage = 0, mmin = 0, mmax = 0;
        texture_util::GetSubresourcesFromFetchConstant(dfetch, &w1, &h1, &d1,
            &bpage, &mpage, &mmin, &mmax);
        const uint32_t guest_base = (bpage << 12) & 0x1fffffff;
        const auto resolved = resolved_textures.find(guest_base);
        std::fprintf(stderr,
            "[metal4-composite] tex fetch=%u dim=%u signed=%u base=%08x phys=%p fmt=%u "
            "size=%ux%u depth=%u pitch=%u tiled=%u endian=%u swizzle=%03x stacked=%u "
            "packed_mips=%u mip=%u..%u host=%ux%u signs=%08x swz=%08x\n",
            binding.fetch_constant, uint32_t(dfetch.dimension), binding.is_signed ? 1u : 0u,
            guest_base, memory.TranslatePhysical(guest_base), uint32_t(dfetch.format),
            w1 + 1, h1 + 1, d1 + 1, dfetch.pitch, uint32_t(dfetch.tiled),
            uint32_t(dfetch.endianness), uint32_t(dfetch.swizzle), uint32_t(dfetch.stacked),
            uint32_t(dfetch.packed_mips), mmin, mmax, bound->width(), bound->height(),
            constants.texture_swizzled_signs[binding.fetch_constant >> 2],
            constants.texture_swizzles[binding.fetch_constant >> 1]);
        if (resolved != resolved_textures.end()) {
          const auto& r = resolved->second;
          const auto& dfi = *FormatInfo::Get(GetBaseFormat(dfetch.format));
          const uint32_t fetch_pitch_bytes = dfetch.pitch * 32 * (dfi.bits_per_pixel / 8);
          const uint32_t fetch_pitch_units = dfetch.pitch * 32;
          std::fprintf(stderr,
              "[metal4-composite] resolved dest=%08x src_base=%u fmt=%u %ux%u pitch=%u "
              "block_bytes=%u tiled=%u clear=%u first=%08x fetch_pitch_units=%u "
              "fetch_pitch_bytes=%u %s %s\n", r.dest, r.src_base, r.format, r.width,
              r.height, r.pitch, r.block_bytes, r.tiled ? 1u : 0u, r.clear ? 1u : 0u,
              r.first, fetch_pitch_units, fetch_pitch_bytes,
              (r.pitch == fetch_pitch_units) ? "PITCH-MATCH" : "PITCH-MISMATCH",
              (r.tiled && dfetch.tiled) ? "TILED-MATCH" : "TILED-MISMATCH");
        } else {
          std::fprintf(stderr, "[metal4-composite] resolved NONE for base=%08x\n", guest_base);
        }
      }
      draw.textures.push_back({stage, set, slot++, bound});
    }
    for (const auto& binding : shader.GetSamplerBindingsAfterTranslation()) {
      const auto fetch = regs.GetTextureFetch(binding.fetch_constant);
      const auto mag = binding.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter : binding.mag_filter;
      const auto min = binding.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter : binding.min_filter;
      auto mip = binding.mip_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mip_filter : binding.mip_filter;
      auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.aniso_filter;
      Need(uint32_t(aniso) <= 5, "invalid guest anisotropy");
      SamplerDesc state;
      state.min_filter = min == xenos::TextureFilter::kLinear ? Filter::kLinear : Filter::kNearest;
      state.mag_filter = mag == xenos::TextureFilter::kLinear ? Filter::kLinear : Filter::kNearest;
      state.mip_filter = uint32_t(mip); state.min_lod = fetch.mip_min_level; state.max_lod = fetch.mip_max_level;
      state.anisotropy = uint32_t(aniso) ? 1u << (uint32_t(aniso) - 1) : 1;
      if (uint32_t(aniso)) { state.min_filter = state.mag_filter = Filter::kLinear; state.mip_filter = 1; }
      auto address = [](xenos::ClampMode mode) {
        switch (mode) {
          case xenos::ClampMode::kRepeat: return AddressMode::kRepeat;
          case xenos::ClampMode::kMirroredRepeat: return AddressMode::kMirrorRepeat;
          case xenos::ClampMode::kClampToEdge:
          case xenos::ClampMode::kClampToHalfway: return AddressMode::kClampEdge;
          case xenos::ClampMode::kMirrorClampToEdge:
          case xenos::ClampMode::kMirrorClampToHalfway:
          case xenos::ClampMode::kMirrorClampToBorder: return AddressMode::kMirrorClampEdge;
          case xenos::ClampMode::kClampToBorder: return AddressMode::kClampBorder;
        }
        throw std::runtime_error("invalid guest sampler addressing");
      };
      xenos::ClampMode u,v,w; texture_util::GetClampModesForDimension(fetch,u,v,w);
      state.u = address(u); state.v = address(v); state.w = address(w); state.border = uint32_t(fetch.border_color);
      Need(state.border < 2 || (state.u != AddressMode::kClampBorder && state.v != AddressMode::kClampBorder && state.w != AddressMode::kClampBorder), "guest custom border color unsupported");
      if (trace_composite && stage == Stage::kFragment) {
        std::fprintf(stderr,
            "[metal4-composite] sampler fetch=%u mag=%u min=%u mip=%u aniso=%u min_lod=%u "
            "max_lod=%u u=%u v=%u w=%u border=%u\n", binding.fetch_constant,
            uint32_t(state.mag_filter), uint32_t(state.min_filter), state.mip_filter,
            state.anisotropy, state.min_lod, state.max_lod, uint32_t(state.u),
            uint32_t(state.v), uint32_t(state.w), state.border);
      }
      SamplerBinding sampler{stage, set, slot++}; sampler.state = state; draw.samplers.push_back(sampler);
    }
  };
  if (trace_composite) {
    std::fprintf(stderr,
        "[metal4-composite] draw present=%u ps=%016llx vs=%016llx color_base=%u fmt=%u "
        "mask=%04x viewport=%u,%u %ux%u scissor=%u,%u %ux%u surface_pitch=%u msaa=%u used=%08x\n",
        frontbuffer_present + 1, static_cast<unsigned long long>(ps_hash),
        static_cast<unsigned long long>(vs->ucode_data_hash()), color_info.color_base,
        uint32_t(color_info.color_format), mask, viewport.xy_offset[0], viewport.xy_offset[1],
        viewport.xy_extent[0], viewport.xy_extent[1], scissor.offset[0], scissor.offset[1],
        scissor.extent[0], scissor.extent[1], surface.surface_pitch,
        uint32_t(surface.msaa_samples), used_textures);
  }
  bind(*vs, Stage::kVertex);
  if (ps) bind(*ps, Stage::kFragment);
  else {
    draw.buffers.push_back({Stage::kFragment, 0, 1, edram_buffer});
    draw.buffers.push_back({Stage::kFragment, 1, Translator::kConstantBufferSystem, system.buffer, system.offset});
  }
  if (count_fragments) {
    if (!fragment_counters) fragment_counters = host.CreateBufferSized(16, "Composite fragment counters");
    draw.buffers.push_back({Stage::kFragment, UINT32_MAX, 30, fragment_counters});
  }
  if (composite_ps && ps) {
    composite_textures.clear(); composite_texture_bases.clear();
    composite_ps_hash = ps_hash;
    for (const auto& binding : ps->GetTextureBindingsAfterTranslation()) {
      const auto dfetch = regs.GetTextureFetch(binding.fetch_constant);
      composite_textures.push_back(textures.at(binding.fetch_constant * 2 + binding.is_signed));
      composite_texture_bases.push_back(uint32_t(dfetch.base_address) << 12);
    }
    const char* dump_directory = std::getenv("MGR_METAL4_DUMP_COMPOSITE");
    const uint32_t dump_from = EnvU32("MGR_METAL4_DUMP_FROM", 0);
    const uint32_t dump_count = EnvU32("MGR_METAL4_DUMP_COUNT", 1);
    const uint32_t present = frontbuffer_present + 1;
    if (dump_composite && dump_directory && present >= dump_from &&
        present < dump_from + dump_count) {
      std::filesystem::create_directories(dump_directory);
      for (size_t i = 0; i < composite_textures.size(); ++i) {
        try {
          auto pixels = host.Readback(composite_textures[i]);
          const uint32_t tw = composite_textures[i]->width();
          const uint32_t th = composite_textures[i]->height();
          char path[1024];
          std::snprintf(path, sizeof(path), "%s/present-%u-ps-%016llx-in%zu.ppm",
                        dump_directory, present,
                        static_cast<unsigned long long>(ps_hash), i);
          std::FILE* file = std::fopen(path, "wb");
          if (file) {
            std::fprintf(file, "P6\n%u %u\n255\n", tw, th);
            for (uint32_t y = 0; y < th; ++y) for (uint32_t x = 0; x < tw; ++x) {
              const size_t p = (size_t(y) * tw + x) * 4;
              std::fputc(pixels[p], file); std::fputc(pixels[p + 1], file);
              std::fputc(pixels[p + 2], file);
            }
            std::fclose(file);
            std::fprintf(stderr, "[metal4-composite] dumped %s (%ux%u)\n", path, tw, th);
          }
        } catch (const std::exception& e) {
          std::fprintf(stderr, "[metal4-composite] input %zu readback failed: %s\n", i, e.what());
        }
      }
    }
    if (skip_this) {
      std::fprintf(stderr, "[metal4-composite] skipping draw ps=%016llx present=%u\n",
                   static_cast<unsigned long long>(ps_hash), present);
      return;
    }
  }
  draw.viewport = {viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0], viewport.xy_extent[1]};
  draw.scissor = {scissor.offset[0], scissor.offset[1], std::min(scissor.extent[0], width - scissor.offset[0]), scissor.extent[1]};
  if (mask) {
    last_draw_by_base[color_info.color_base] = {color_info.color_base,
        uint32_t(color_info.color_format), mask,
        static_cast<uint64_t>(ps ? ps->ucode_data_hash() : 0)};
  }
  if (std::getenv("MGR_METAL4_TRACE_FRAME") && frontbuffer_present + 1 >= 148 &&
      frontbuffer_present + 1 <= 152 && mask) {
    std::fprintf(stderr,
        "[metal4-frame] draw base=%u fmt=%u mask=%04x ps=%llx prim=%u count=%u "
        "scissor=%u,%u %ux%u\n", color_info.color_base,
        uint32_t(color_info.color_format), mask,
        static_cast<unsigned long long>(ps ? ps->ucode_data_hash() : 0),
        uint32_t(primitive), count, scissor.offset[0], scissor.offset[1],
        scissor.extent[0], scissor.extent[1]);
  }
  if (trace_draw && color_info.color_base == 0 && mask) {
    static uint32_t base0_logged = 0;
    if (base0_logged++ < 8) {
      const auto fetch0_log = regs.GetTextureFetch(0);
      uint32_t used_texture_mask_log = vs->GetUsedTextureMaskAfterTranslation() |
          (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
      std::fprintf(stderr,
          "[metal4-xenos] base0 draw prim=%u count=%u scissor=%u,%u %ux%u viewport=%u,%u %ux%u "
          "target=%ux%u vs=%llu ps=%llu fetch0=%08x fmt=%u %ux%u tiled=%u used=%08x\n",
          uint32_t(primitive), count, scissor.offset[0], scissor.offset[1], scissor.extent[0],
          scissor.extent[1], viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0],
          viewport.xy_extent[1], width, height, static_cast<unsigned long long>(vs->ucode_data_hash()),
          static_cast<unsigned long long>(ps ? ps->ucode_data_hash() : 0),
          uint32_t(fetch0_log.base_address) << 12, uint32_t(fetch0_log.format),
          fetch0_log.size_2d.width + 1, fetch0_log.size_2d.height + 1, uint32_t(fetch0_log.tiled),
          used_texture_mask_log);
    }
  }
  draw.depth_min = viewport.z_min; draw.depth_max = viewport.z_max;
  auto raster = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  draw.cull_front = polygonal && raster.cull_front; draw.cull_back = polygonal && raster.cull_back; draw.front_clockwise = !raster.face;
  draw.blend_constant = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN), regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  PassDesc pass; pass.attachmentless_width = width; pass.attachmentless_height = height; pass.raster_samples = pipeline.raster_samples; pass.label = pipeline.label;
  if (pass.raster_samples == 4) for (const auto& p : draw_util::kD3D10StandardSamplePositions4x)
    pass.sample_positions.push_back({0.5f + p[0] / 16.0f, 0.5f + p[1] / 16.0f});
  // First encounters block for correctness; the host caches the PSO and archive
  // for subsequent draws/launches. No placeholder shader is substituted.
  draw.pipeline = host.PreparePipeline(std::move(pipeline)).get();
  const bool trace_output = std::getenv("MGR_METAL4_TRACE_OUTPUT") &&
      frontbuffer_present + 1 >= EnvU32("MGR_METAL4_TRACE_OUTPUT_FROM", 0) &&
      frontbuffer_present + 1 <= EnvU32("MGR_METAL4_TRACE_OUTPUT_TO", UINT32_MAX);
  const char* trace_output_ps = std::getenv("MGR_METAL4_TRACE_OUTPUT_PS");
  const bool trace_output_target = !trace_output_ps || !*trace_output_ps ||
      std::strtoull(trace_output_ps, nullptr, 16) == ps_hash;
  const uint32_t output_center_tile = (color_info.color_base +
      (height / 2 / 16) * xenos::GetSurfacePitchTiles(width, surface.msaa_samples, false) +
      (width / 2 / 80)) & 2047;
  const size_t rt_offset = size_t(output_center_tile) * 5120;
  uint64_t output_before = 0;
  static std::set<uint64_t> counted_composites;
  const bool count_this_draw = count_fragments && !counted_composites.contains(ps_hash);
  std::vector<std::byte> edram_before;
  if (count_this_draw) {
    const std::array<uint32_t, 4> zero{};
    host.UpdateBuffer(fragment_counters, 0, std::as_bytes(std::span(zero)));
    edram_before = host.ReadBuffer(edram_buffer);
  }
  if (trace_output && trace_output_target) {
    const auto bytes = host.ReadBufferRange(edram_buffer, rt_offset, 5120);
    output_before = XXH3_64bits(bytes.data(), bytes.size());
  }
  Need(host.BeginFrame(false), "could not begin guest draw");
  host.BeginPass(pass); host.Draw(draw); host.EndPass(); host.Submit();
  if (count_this_draw) {
    const auto values = host.ReadBuffer(fragment_counters);
    const auto after = host.ReadBuffer(edram_buffer);
    std::array<uint32_t, 4> counters{};
    std::memcpy(counters.data(), values.data(), sizeof(counters));
    size_t changed = 0;
    for (size_t i = 0; i < after.size(); ++i) changed += edram_before[i] != after[i];
    std::fprintf(stderr,
        "[metal4-fragments] ps=%016llx draw=%llu entered=%u edram_access=%u "
        "edram_stores=%u edram_bytes_changed=%zu primitive=%u count=%u indexed=%u\n",
        static_cast<unsigned long long>(ps_hash), static_cast<unsigned long long>(draw_number),
        counters[0], counters[1], counters[2], changed, uint32_t(draw.primitive),
        draw.count, draw.indices ? 1u : 0u);
    counted_composites.insert(ps_hash);
  }
  if (trace_output && trace_output_target) {
    const auto bytes = host.ReadBufferRange(edram_buffer, rt_offset, 5120);
    const auto full_edram = host.ReadBuffer(edram_buffer);
    std::fprintf(stderr,
        "[metal4-output] present=%u draw=%llu ps=%016llx rt0_base=%u center_tile=%u "
        "center_before=%016llx center_after=%016llx edram=%016llx\n",
        frontbuffer_present + 1, static_cast<unsigned long long>(draw_number),
        static_cast<unsigned long long>(ps_hash), color_info.color_base, output_center_tile,
        static_cast<unsigned long long>(output_before),
        static_cast<unsigned long long>(XXH3_64bits(bytes.data(), bytes.size())),
        static_cast<unsigned long long>(XXH3_64bits(full_edram.data(), full_edram.size())));
  }
  // Diagnostic only: replace each enabled 32-bpp RT with a distinct color
  // after the fragment pass. This probes the EDRAM route consumed by later
  // resolves and composites without changing normal shader translation.
  const char* override_ps = std::getenv("MGR_METAL4_OVERRIDE_PS");
  if (ps && override_ps && *override_ps &&
      std::strtoull(override_ps, nullptr, 16) == ps->ucode_data_hash()) {
    host.WaitIdle();
    auto bytes = host.ReadBuffer(edram_buffer);
    constexpr uint32_t colors[4] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffff00ff};
    const uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(width, surface.msaa_samples, false);
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((mask >> (i * 4)) & 15)) continue;
      const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      if (info.color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) continue;
      for (uint32_t y = 0; y < height; y += 16) {
        for (uint32_t x = 0; x < width; x += 80) {
          const uint32_t tile = (info.color_base + (y / 16) * pitch_tiles + x / 80) & 2047;
          auto* words = reinterpret_cast<uint32_t*>(bytes.data() + size_t(tile) * 5120);
          std::fill_n(words, 1280, colors[i]);
        }
      }
      std::fprintf(stderr, "[metal4-override] ps=%016llx logical_rt=%u edram_base=%u color=%08x\n",
          static_cast<unsigned long long>(ps->ucode_data_hash()), i, info.color_base, colors[i]);
    }
    host.UpdateBuffer(edram_buffer, 0, std::span<const std::byte>(bytes));
  }
  if (trace_scene && mask) {
    host.WaitIdle();
    for (uint32_t i = 0; i < 4; ++i) if ((mask >> (i * 4)) & 15) {
      const auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      const uint32_t center_tile = (info.color_base +
          (height / 2 / 16) * xenos::GetSurfacePitchTiles(width, surface.msaa_samples, false) +
          (width / 2 / 80)) & 2047;
      const auto tile = host.ReadBufferRange(edram_buffer, size_t(center_tile) * 5120, 5120);
      std::fprintf(stderr, "[metal4-pm4] draw=%llu rt=%u base=%u center_tile=%u xxh3=%016llx\n",
          static_cast<unsigned long long>(draw_number), i, info.color_base, center_tile,
          static_cast<unsigned long long>(XXH3_64bits(tile.data(), tile.size())));
    }
  }
}
void MetalCommandProcessor::GuestRenderer::Resolve() {
  ScopeTimer timer(resolve_us);
  draw_util::ResolveInfo info{};
  Need(draw_util::GetResolveInfo(regs, memory, 1, 1, false, false, info), "invalid Xenos resolve registers");
  const uint32_t width = info.coordinate_info.width_div_8 * 8, height = info.height_div_8 * 8;
  if (!width || !height) return;
  const auto& dest = info.copy_dest_info;
  const auto source_info = info.IsCopyingDepth() ? info.depth_edram_info : info.color_edram_info;
  const bool copying = info.copy_dest_extent_length && info.rb_copy_control.copy_command != xenos::CopyCommand::kNull;
  const bool depth_copy = info.IsCopyingDepth();
  Need(!dest.copy_dest_array, "guest volume resolve unsupported");
  const auto dest_format = xenos::TextureFormat(dest.copy_dest_format);
  const auto& format_info = *FormatInfo::Get(dest_format);
  const uint32_t dest_bytes = depth_copy ? 4 : format_info.bits_per_pixel / 8;
  Need(!copying || (dest_bytes && dest_bytes <= 16 && std::has_single_bit(dest_bytes)), "unsupported resolve destination size");
  // Fast exact copy for matching formats. Conversion is kept separate so no
  // accidental reinterpretation can turn float depth/color into normalized data.
  const bool bitwise = depth_copy || xenos::IsColorResolveFormatBitwiseEquivalent(xenos::ColorRenderTargetFormat(source_info.format), xenos::ColorFormat(dest.copy_dest_format));

  const uint32_t dest_pitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
  const uint32_t dest_bpe_log2 = std::countr_zero(dest_bytes);
  const uint32_t xor_mask[] = {0, 1, 3, 2, 7, 15};
  Need(uint32_t(dest.copy_dest_endian) < std::size(xor_mask), "unknown resolve byte order");
  const uint32_t endian_mask = xor_mask[uint32_t(dest.copy_dest_endian)];
  const uint32_t selection = uint32_t(info.copy_dest_coordinate_info.copy_sample_select);
  const uint32_t first_sample = selection < 4 ? selection : selection == 5 ? 2 : 0;
  const uint32_t sample_count = selection < 4 ? 1 : selection == 6 ? 4 : 2;
  const bool format_8 = xenos::ColorRenderTargetFormat(source_info.format) ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
      dest_format == xenos::TextureFormat::k_8_8_8_8;
  const uint32_t x_shift = source_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  const uint32_t y_shift = source_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  // The GPU path covers bitwise 32bpp copies and the 8_8_8_8 sample average.
  // Clears, format conversion, depth and wide byte orders stay on the CPU
  // oracle. MGR_METAL4_RESOLVE_CPU=1 forces the CPU path for A/B runs.
  // Depth is a bitwise 32-bit copy here; sample expansion and conversion stay
  // on the CPU oracle.
  const bool depth_ok = depth_copy && sample_count == 1 && bitwise && dest_bytes == 4;
  const bool color_ok = !depth_copy && (sample_count == 1 || format_8);
  const char* cpu_base_value = std::getenv("MGR_METAL4_RESOLVE_CPU_BASE");
  const bool force_cpu_base = cpu_base_value &&
      std::strtoul(cpu_base_value, nullptr, 0) == source_info.base_tiles;
  const bool gpu_resolve = copying && bitwise && dest_bytes == 4 &&
      !dest.copy_dest_exp_bias && !info.IsClearingColor() && !info.IsClearingDepth() &&
      (depth_ok || color_ok) &&
      !source_info.format_is_64bpp && uint32_t(dest.copy_dest_endian) < 4 &&
      edram_buffer && shared_memory && !force_cpu_base &&
      std::getenv("MGR_METAL4_RESOLVE_CPU") == nullptr &&
      std::getenv("MGR_METAL4_TRACE_EDRAM") == nullptr;
  if (gpu_resolve) {
    EdramResolveParams p{};
    p.width = width; p.height = height;
    p.src_base_tiles = source_info.base_tiles;
    p.src_pitch_tiles = source_info.pitch_tiles;
    p.dest_base = info.copy_dest_base;
    p.dest_pitch = dest_pitch;
    p.src_offset_x = info.coordinate_info.edram_offset_x_div_8 * 8;
    p.src_offset_y = info.coordinate_info.edram_offset_y_div_8 * 8;
    p.dest_offset_x = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
    p.dest_offset_y = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    p.endian_xor = endian_mask;
    p.swap_rb = dest.copy_dest_swap ? 1 : 0;
    p.sample_count = sample_count;
    p.first_sample = first_sample;
    p.x_shift = x_shift;
    p.y_shift = y_shift;
    p.is_depth = depth_copy ? 1 : 0;
    const auto begin = std::chrono::steady_clock::now();
    host.ResolveEdram(edram_buffer, shared_memory, p);
    // The shader writes the shared guest-memory mirror. Publish only the
    // resolved range back to the authoritative guest allocation; texture
    // uploads and the CPU oracle continue to read that allocation.
    const uint64_t copy_base = info.copy_dest_base;
    const uint64_t copy_length = info.copy_dest_extent_length ?
        std::min<uint64_t>(info.copy_dest_extent_length, (512ull << 20) - copy_base) : 0;
    if (copy_length) {
      auto* destination = memory.TranslatePhysical(uint32_t(copy_base));
      host.CopyBufferToHost(shared_memory, copy_base, size_t(copy_length), destination);
      texture_dirty_current.emplace_back(copy_base, copy_base + copy_length);
      InvalidateTextureHashes(copy_base, copy_length);
      if (std::getenv("MGR_METAL4_TRACE_RESOLVE") && resolve_gpu_count < 32) {
        uint32_t word = 0; std::memcpy(&word, destination, 4);
        std::fprintf(stderr, "[metal4-xenos] resolve-gpu %llu %ux%u src_base=%u dest=%08x depth=%u first=%08x\n",
                     static_cast<unsigned long long>(resolve_gpu_count + 1), width, height,
                     source_info.base_tiles, info.copy_dest_base, depth_copy ? 1u : 0u, word);
      }
      if (std::getenv("MGR_METAL4_TRACE_FRAME") && frontbuffer_present + 1 >= 148 &&
          frontbuffer_present + 1 <= 152) {
        uint32_t word = 0; std::memcpy(&word, destination, 4);
        std::fprintf(stderr, "[metal4-frame] resolve src_base=%u dest=%08x fmt=%u depth=%u "
                     "clear=%u/%u first=%08x\n", source_info.base_tiles, info.copy_dest_base,
                     uint32_t(source_info.format), depth_copy ? 1u : 0u,
                     uint32_t(info.IsClearingColor()), uint32_t(info.IsClearingDepth()), word);
      }
      ResolvedTextureInfo resolved{};
      resolved.src_base = source_info.base_tiles;
      resolved.dest = info.copy_dest_base;
      resolved.format = uint32_t(dest_format);
      resolved.width = width; resolved.height = height;
      resolved.pitch = dest_pitch;
      resolved.block_bytes = dest_bytes;
      resolved.offset_x = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
      resolved.offset_y = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
      // The resolve writes a tiled destination at this pitch.
      resolved.tiled = true;
      resolved.clear = info.IsClearingColor() || info.IsClearingDepth();
      std::memcpy(&resolved.first, destination, 4);
      resolved_textures[info.copy_dest_base] = resolved;
    }
    resolve_gpu_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
    ++resolve_gpu_count;
    return;
  }
  if (std::getenv("MGR_METAL4_TRACE_RESOLVE") && copying) {
    static std::set<std::string> reasons;
    std::string reason;
    if (!edram_buffer || !shared_memory) reason = "missing buffers";
    else if (force_cpu_base) reason = "forced CPU base";
    else if (std::getenv("MGR_METAL4_RESOLVE_CPU")) reason = "forced CPU";
    else if (std::getenv("MGR_METAL4_TRACE_EDRAM")) reason = "EDRAM trace";
    else if (depth_copy) reason = "depth";
    else if (!bitwise) reason = "format conversion";
    else if (dest_bytes != 4) reason = "dest bytes";
    else if (dest.copy_dest_exp_bias) reason = "exp bias";
    else if (info.IsClearingColor()) reason = "color clear";
    else if (info.IsClearingDepth()) reason = "depth clear";
    else if (sample_count > 1 && !format_8) reason = "sample average format";
    else if (source_info.format_is_64bpp) reason = "64bpp source";
    else if (uint32_t(dest.copy_dest_endian) >= 4) reason = "wide byte order";
    else reason = "other";
    if (reasons.insert(reason).second)
      std::fprintf(stderr, "[metal4-xenos] resolve-cpu reason=%s\n", reason.c_str());
  }
  FlushColor();
  static uint64_t resolve_index = 0;
  const uint64_t resolve_number = ++resolve_index;
  const bool trace = std::getenv("MGR_METAL4_TRACE_EDRAM") != nullptr;
  auto address = [&](draw_util::ResolveEdramInfo surface, uint32_t x, uint32_t y, uint32_t sample) {
    x += info.coordinate_info.edram_offset_x_div_8 * 8; y += info.coordinate_info.edram_offset_y_div_8 * 8;
    x = (x << (surface.msaa_samples >= xenos::MsaaSamples::k4X)) + (sample >> 1);
    y = (y << (surface.msaa_samples >= xenos::MsaaSamples::k2X)) + (sample & 1);
    const uint32_t tile_width = surface.format_is_64bpp ? 40 : 80;
    const uint32_t tile = (surface.base_tiles + (y / 16) * surface.pitch_tiles + x / tile_width) & 2047;
    uint32_t column = x % tile_width;
    if (surface.is_depth) column = column < 40 ? column + 40 : column - 40;
    return size_t(tile) * 5120 + ((y % 16) * 80 + (column << surface.format_is_64bpp)) * 4;
  };
  const uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8, dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    if (copying) {
      const uint32_t selection = uint32_t(info.copy_dest_coordinate_info.copy_sample_select);
      const uint32_t first_sample = selection < 4 ? selection : selection == 5 ? 2 : 0;
      const uint32_t sample_count = selection < 4 ? 1 : selection == 6 ? 4 : 2;
      const size_t source = address(source_info, x, y, first_sample);
      std::array<uint8_t,16> converted{};
      // A 32-bit RB swap is a byte permutation, not a format conversion.
      // Wider formats must still go through the channel-aware slow path.
      if (bitwise && sample_count == 1 && !dest.copy_dest_exp_bias &&
          (!dest.copy_dest_swap || dest_bytes == 4)) {
        Need(source + dest_bytes <= edram.size(), "resolve source exceeds EDRAM");
        std::memcpy(converted.data(), edram.data() + source, dest_bytes);
        if (dest.copy_dest_swap) std::swap(converted[0], converted[2]);
      } else {
        Need(!depth_copy, "depth resolve cannot average samples");
        std::array<float,4> sum{};
        for (uint32_t sample = first_sample; sample < first_sample + sample_count; ++sample) {
          const auto value = UnpackColor(xenos::ColorRenderTargetFormat(source_info.format), edram.data() + address(source_info, x, y, sample));
          for (uint32_t c = 0; c < 4; ++c) sum[c] += value[c];
        }
        for (auto& c : sum) c *= std::ldexp(1.0f / sample_count, dest.copy_dest_exp_bias);
        if (dest.copy_dest_swap) std::swap(sum[0], sum[2]);
        PackColor(dest_format, sum, converted.data());
      }
      const uint32_t offset = uint32_t(TiledOffset2D(int32_t(dx + x), int32_t(dy + y), dest_pitch, dest_bpe_log2));
      const uint64_t target = uint64_t(info.copy_dest_base) + offset;
      Need(target + dest_bytes <= 512 * 1024 * 1024, "resolve destination exceeds physical memory");
      Need(source + dest_bytes <= edram.size(), "resolve source exceeds EDRAM");
      if (endian_mask < 4) {
        // XOR with 0/1/2/3 is a permutation inside this 4-byte word, so one
        // translation covers all destination bytes.
        auto* dst = memory.TranslatePhysical(uint32_t(target));
        for (uint32_t byte = 0; byte < dest_bytes; ++byte) dst[byte ^ endian_mask] = converted[byte];
      } else {
        for (uint32_t byte = 0; byte < dest_bytes; ++byte)
          *memory.TranslatePhysical(uint32_t(target + byte) ^ endian_mask) = converted[byte];
      }
    }
    if (info.IsClearingColor()) {
      const uint32_t samples = 1u << uint32_t(info.color_edram_info.msaa_samples);
      for (uint32_t sample = 0; sample < samples; ++sample) {
        const auto source = address(info.color_edram_info, x, y, sample);
        std::memcpy(edram.data() + source, &info.rb_color_clear, 4);
        if (info.color_edram_info.format_is_64bpp) std::memcpy(edram.data() + source + 4, &info.rb_color_clear_lo, 4);
      }
    }
    if (info.IsClearingDepth()) {
      const uint32_t samples = 1u << uint32_t(info.depth_edram_info.msaa_samples);
      for (uint32_t sample = 0; sample < samples; ++sample)
        std::memcpy(edram.data() + address(info.depth_edram_info, x, y, sample), &info.rb_depth_clear, 4);
    }
  }
  if (trace && resolve_number <= 400) {
    const uint32_t edram_nonzero = CountNonzeroDwords(edram.data(), edram.size());
    uint32_t source_nonzero = 0;
    {
      const uint64_t source_byte = uint64_t(source_info.base_tiles) * 5120;
      if (source_byte < edram.size()) {
        const uint64_t rows = uint64_t(source_info.pitch_tiles) * 16 *
            (uint64_t(info.IsCopyingDepth() ? info.depth_edram_info.pitch_tiles : 0));
        (void)rows;
        const uint64_t source_bytes = std::min<uint64_t>(edram.size() - source_byte, 4ull << 20);
        source_nonzero = CountNonzeroDwords(edram.data() + source_byte,
                                            static_cast<size_t>(source_bytes));
      }
    }
    uint32_t dest_nonzero = 0, dest_rgb = 0, dest_alpha = 0;
    if (copying && info.copy_dest_extent_length) {
      const uint32_t length = std::min<uint32_t>(info.copy_dest_extent_length, 32u << 20);
      const auto* dest_bytes = memory.TranslatePhysical(info.copy_dest_extent_start);
      dest_nonzero = CountNonzeroDwords(reinterpret_cast<const uint8_t*>(dest_bytes), length);
      for (uint32_t i = 0; i + 4 <= length; i += 4) {
        uint32_t word;
        std::memcpy(&word, reinterpret_cast<const uint8_t*>(dest_bytes) + i, 4);
        dest_rgb += (word & 0x00ffffffu) != 0;
        dest_alpha += (word >> 24) != 0;
      }
    }
    std::fprintf(stderr,
        "[metal4-xenos] resolve %llu %ux%u copy=%u depth=%u src_fmt=%u src_base=%u pitch=%u "
        "dest=%08x dest_fmt=%u len=%08x start=%08x clear=%u/%u clear_color=%08x/%08x "
        "dest_endian=%u dest_swap=%u first=%08x edram_nonzero=%u source_nonzero=%u "
        "dest_nonzero=%u dest_rgb=%u dest_alpha=%u\n",
        static_cast<unsigned long long>(resolve_number), width, height, uint32_t(copying),
        uint32_t(depth_copy), uint32_t(source_info.format), uint32_t(source_info.base_tiles),
        uint32_t(source_info.pitch_tiles), info.copy_dest_base, uint32_t(dest.copy_dest_format),
        info.copy_dest_extent_length, info.copy_dest_extent_start,
        uint32_t(info.IsClearingColor()), uint32_t(info.IsClearingDepth()),
        info.rb_color_clear, info.rb_color_clear_lo,
        uint32_t(dest.copy_dest_endian), uint32_t(dest.copy_dest_swap),
        [&] { uint32_t word = 0; if (copying && info.copy_dest_extent_length)
                 std::memcpy(&word, memory.TranslatePhysical(info.copy_dest_extent_start), 4);
              return word; }(),
        edram_nonzero, source_nonzero, dest_nonzero, dest_rgb, dest_alpha);
  }
  if (copying && info.copy_dest_extent_length) {
    texture_dirty_current.emplace_back(info.copy_dest_base,
                                       uint64_t(info.copy_dest_base) + info.copy_dest_extent_length);
    InvalidateTextureHashes(info.copy_dest_base, info.copy_dest_extent_length);
    ResolvedTextureInfo resolved{};
    resolved.src_base = source_info.base_tiles;
    resolved.dest = info.copy_dest_base;
    resolved.format = uint32_t(dest_format);
    resolved.width = width; resolved.height = height;
    resolved.pitch = dest_pitch;
    resolved.block_bytes = dest_bytes;
    resolved.offset_x = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
    resolved.offset_y = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    resolved.tiled = true;
    resolved.clear = info.IsClearingColor() || info.IsClearingDepth();
    if (info.copy_dest_extent_length >= 4)
      std::memcpy(&resolved.first, memory.TranslatePhysical(info.copy_dest_extent_start), 4);
    resolved_textures[info.copy_dest_base] = resolved;
    if (std::getenv("MGR_METAL4_TRACE_FRAME") && frontbuffer_present + 1 >= 148 &&
        frontbuffer_present + 1 <= 152) {
      std::fprintf(stderr,
          "[metal4-frame] resolve-cpu src_base=%u dest=%08x fmt=%u depth=%u clear=%u/%u first=%08x\n",
          resolved.src_base, resolved.dest, resolved.format, depth_copy ? 1u : 0u,
          uint32_t(info.IsClearingColor()), uint32_t(info.IsClearingDepth()), resolved.first);
    }
  }
  if (edram_buffer && (info.IsClearingColor() || info.IsClearingDepth())) {
    host.WaitIdle(); host.UpdateBuffer(edram_buffer, 0, std::as_bytes(std::span(edram)));
  }
}
std::shared_ptr<Target> MetalCommandProcessor::GuestRenderer::FrontBuffer(uint32_t width, uint32_t height) {
  ScopeTimer timer(frontbuffer_us);
  // The shared Xenos frontend communicates the frontbuffer through fetch 0,
  // matching RequestSwapTexture in the Vulkan reference.
  const auto fetch = regs.GetTextureFetch(0);
  texture_dirty_prev = std::move(texture_dirty_current);
  texture_dirty_current.clear();
  ++guest_frame;
  static uint32_t presented = 0;
  const uint32_t present = presented++;
  frontbuffer_present = present;
  // Diagnostic views: present the resolved scene or one of the final
  // composite's inputs instead of the game's frontbuffer. Off by default so
  // render-target selection and presentation are unchanged.
  const char* viz = std::getenv("MGR_METAL4_VIZ");
  bool viz_active = false;
  std::shared_ptr<Target> array;
  if (viz && !std::strcmp(viz, "scene")) {
    const ResolvedTextureInfo* scene = nullptr;
    for (const auto& base : composite_texture_bases) {
      const auto it = resolved_textures.find(base);
      if (it != resolved_textures.end() && it->second.src_base == 1328) {
        scene = &it->second; break;
      }
    }
    if (!scene) {
      uint32_t scene_area = 0;
      for (const auto& [dest, info] : resolved_textures) {
        if (info.src_base != 1328) continue;
        const uint32_t area = info.width * info.height;
        if (!scene || area > scene_area) { scene = &info; scene_area = area; }
      }
    }
    if (scene && scene->width && scene->height && scene->block_bytes == 4) {
      std::vector<uint8_t> pixels(size_t(scene->width) * scene->height * 4);
      const uint32_t bpe_log2 = std::countr_zero(scene->block_bytes);
      for (uint32_t y = 0; y < scene->height; ++y) for (uint32_t x = 0; x < scene->width; ++x) {
        const uint32_t offset = uint32_t(TiledOffset2D(
            int32_t(scene->offset_x + x), int32_t(scene->offset_y + y), scene->pitch, bpe_log2));
        const uint64_t address = uint64_t(scene->dest) + offset;
        Need(address + 4 <= 512 * 1024 * 1024, "scene resolve exceeds physical memory");
        std::memcpy(pixels.data() + (size_t(y) * scene->width + x) * 4,
                    memory.TranslatePhysical(uint32_t(address)), 4);
      }
      array = host.CreateTextureRGBA8(scene->width, scene->height,
          std::as_bytes(std::span(pixels)), "Xenos scene resolve");
      viz_active = true;
      std::fprintf(stderr, "[metal4-viz] scene resolve dest=%08x %ux%u pitch=%u\n",
                   scene->dest, scene->width, scene->height, scene->pitch);
    }
  } else if (viz && !composite_textures.empty()) {
    size_t index = 0;
    if (!std::strncmp(viz, "input", 5))
      index = std::strtoul(viz + 5, nullptr, 10);
    if (index < composite_textures.size()) {
      array = composite_textures[index];
      viz_active = true;
    }
  }
  if (!array) {
    Need(width <= fetch.size_2d.width + 1 && height <= fetch.size_2d.height + 1,
         "frontbuffer active region exceeds texture extent");
    array = Texture(0);
  }
  if (std::getenv("MGR_METAL4_TRACE_FRAME") && present == 150) {
    for (const auto& [base, info] : last_draw_by_base) {
      if (base != 0 && base != 1328) continue;
      std::fprintf(stderr, "[metal4-frame] last draw base=%u fmt=%u mask=%04x ps=%llx\n",
                   base, info.color_fmt, info.mask,
                   static_cast<unsigned long long>(info.ps_hash));
    }
  }
  const uint32_t first = EnvU32("MGR_METAL4_DUMP_FROM", 0);
  const uint32_t count = EnvU32("MGR_METAL4_DUMP_COUNT", 8);
  const char* directory = std::getenv("MGR_METAL4_DUMP_FRAMES");
  const bool dump = directory && present >= first && present < first + count;
  const uint32_t source_width = viz_active ? array->width() : fetch.size_2d.width + 1;
  const uint32_t out_width = viz_active ? array->width() : width;
  const uint32_t out_height = viz_active ? array->height() : height;
  const bool direct = !viz_active && array->width() == width && array->height() == height;
  const bool diagnostic = dump ||
      (std::getenv("MGR_METAL4_TRACE_FRONTBUFFER") && present % 30 == 0) ||
      (std::getenv("MGR_METAL4_TRACE_EDRAM") && present <= 400);
  std::vector<uint8_t> cropped;
  if (!direct || diagnostic) {
    auto pixels = host.Readback(array);
    cropped.resize(size_t(out_width) * out_height * 4);
    for (uint32_t y = 0; y < out_height; ++y)
      std::memcpy(cropped.data() + size_t(y) * out_width * 4,
                  pixels.data() + size_t(y) * source_width * 4, size_t(out_width) * 4);
  }
  if (diagnostic) {
    uint32_t nonzero = 0;
    for (size_t pixel = 0; pixel < cropped.size(); pixel += 4)
      nonzero += (cropped[pixel] | cropped[pixel+1] | cropped[pixel+2]) != 0;
    std::fprintf(stderr,
        "[metal4-xenos] frontbuffer %ux%u present %u fetch0=%08x %ux%u nonblack=%u/%u direct=%u viz=%s\n",
        out_width, out_height, present, uint32_t(fetch.base_address) << 12,
        fetch.size_2d.width + 1, fetch.size_2d.height + 1, nonzero, out_width * out_height,
        direct ? 1u : 0u, viz_active ? viz : "none");
    if (dump) {
      std::filesystem::create_directories(directory);
      auto path = std::filesystem::path(directory) / ("guest-" + std::to_string(present) + ".ppm");
      std::ofstream output(path, std::ios::binary);
      output << "P6\n" << out_width << " " << out_height << "\n255\n";
      for (size_t pixel = 0; pixel < cropped.size(); pixel += 4)
        output.write(reinterpret_cast<const char*>(cropped.data() + pixel), 3);
      std::fprintf(stderr, "[metal4-xenos] frontbuffer dump %s\n", path.c_str());
    }
  }
  if (direct) return array;
  if (viz_active) return array;
  return host.CreateTextureRGBA8(width, height, std::as_bytes(std::span(cropped)), "Xenos frontbuffer");
}
}
