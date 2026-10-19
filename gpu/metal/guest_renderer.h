#pragma once
#include <set>
#include "runtime.h"
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/util/draw.h>

namespace mgr::gpu::metal {
// Owns Xbox state and address interpretation. The host backend only sees
// ordinary buffers, textures and draw descriptions.
struct MetalCommandProcessor::GuestRenderer {
  using Translator = rex::graphics::SpirvShaderTranslator;
  GuestRenderer(HostBackend&, rex::graphics::RegisterFile&, rex::memory::Memory&);
  void Draw(rex::graphics::xenos::PrimitiveType, uint32_t, const IndexBufferInfo*,
      rex::graphics::SpirvShader*, rex::graphics::SpirvShader*);
  void Resolve();
  std::shared_ptr<Target> FrontBuffer(uint32_t width, uint32_t height);
  // CPU time accumulated on the guest GPU thread, in microseconds.
  uint64_t draw_us = 0, resolve_us = 0, frontbuffer_us = 0;
  uint64_t resolve_gpu_us = 0, resolve_gpu_count = 0;
  uint64_t texture_us = 0, texture_calls = 0, texture_uploads = 0;
  uint64_t texture_lookup_us = 0, texture_translate_us = 0, texture_convert_us = 0;
  uint64_t texture_upload_us = 0, texture_wait_us = 0;
  uint64_t texture_bytes = 0, texture_unique = 0;
  uint64_t texture_cache_hits = 0, texture_cache_misses = 0;
  uint64_t texture_hashes = 0, texture_hash_bytes = 0;
  std::set<std::string> unique_texture_keys;
  struct CachedTextureHash {
    uint64_t base_hash = 0, mip_hash = 0;
    uint64_t frame = UINT64_MAX;
    uint32_t base_page = 0, mip_page = 0;
    uint32_t base_bytes = 0, mip_bytes = 0;
  };
  std::map<std::string, CachedTextureHash> texture_hashes_by_descriptor;
  // Resolve destinations that may have changed guest texture bytes. Ranges from
  // the previous frame are checked by draws; the current frame's are checked by
  // the frontbuffer after its resolves.
  std::vector<std::pair<uint64_t, uint64_t>> texture_dirty_prev, texture_dirty_current;
  uint64_t guest_frame = 0;
  uint32_t frontbuffer_present = 0;
  struct LastDrawInfo { uint32_t color_base = 0, color_fmt = 0, mask = 0; uint64_t ps_hash = 0; };
  std::map<uint32_t, LastDrawInfo> last_draw_by_base;
  // Resources produced by the last resolve to each destination, for comparing
  // what the final composite fetches against what the resolve actually wrote.
  struct ResolvedTextureInfo {
    uint32_t src_base = 0, dest = 0, format = 0, width = 0, height = 0, pitch = 0;
    uint32_t block_bytes = 4, offset_x = 0, offset_y = 0;
    bool tiled = false, clear = false;
    uint32_t first = 0;
  };
  std::map<uint32_t, ResolvedTextureInfo> resolved_textures;
  // Host textures bound by the last final-composite fragment shader, and the
  // guest addresses their fetch constants reference (for visualization only).
  std::vector<std::shared_ptr<Target>> composite_textures;
  std::vector<uint32_t> composite_texture_bases;
  uint64_t composite_ps_hash = 0;
  static constexpr uint64_t kTextureHashInterval = 15;
 private:
  ShaderCode Translate(rex::graphics::SpirvShader&, Translator::Modification);
  void FlushColor();
  void InvalidateTextureHashes(uint64_t start, uint64_t length);
  struct TransientSlice { std::shared_ptr<Buffer> buffer; size_t offset; };
  TransientSlice Transient(std::span<const std::byte> bytes, size_t alignment = 256);
  std::shared_ptr<Target> Texture(uint32_t fetch, bool is_signed = false);
  size_t EdramAddress(uint32_t base, uint32_t pitch_tiles, uint32_t x, uint32_t y) const;
  HostBackend& host;
  rex::graphics::RegisterFile& regs;
  rex::memory::Memory& memory;
  std::unique_ptr<Translator> translator;
  std::shared_ptr<Buffer> shared_memory;
  std::vector<uint8_t> edram;
  std::shared_ptr<Buffer> edram_buffer;
  // Per-draw upload ring. Rewound after the draw fence, so a draw never
  // overwrites bytes the GPU is still reading.
  static constexpr size_t kTransientCapacity = 32 * 1024 * 1024;
  std::shared_ptr<Buffer> transient_ring;
  size_t transient_cursor = 0;
  struct CachedTexture { std::shared_ptr<Target> image; size_t bytes; uint64_t used; };
  std::map<std::string, CachedTexture> texture_cache;
  std::shared_ptr<Target> null_texture;
  std::shared_ptr<Target> diagnostic_texture;
  std::shared_ptr<Buffer> fragment_counters;
  size_t texture_cache_bytes = 0;
  uint64_t texture_clock = 0;
};
}
