#pragma once
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
 private:
  ShaderCode Translate(rex::graphics::SpirvShader&, Translator::Modification);
  void FlushColor();
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
  size_t texture_cache_bytes = 0;
  uint64_t texture_clock = 0;
};
}
