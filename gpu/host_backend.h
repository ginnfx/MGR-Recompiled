#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct SDL_Window;

namespace mgr::gpu {

// Host operations only. PM4, registers, Xenos formats, endian conversion,
// primitive expansion, EDRAM ownership and guest completion stay in Xenos.
enum class Format { kNone, kRGBA8, kBGRA8, kRGBA16Float, kDepth32Stencil8, kR8, kRG8, kR16, kRG16, kRGBA16, kR16Float, kRG16Float, kR32Float, kRG32Float, kRGBA32Float, kBC1, kBC2, kBC3, kBC5, kR8Snorm, kRG8Snorm, kRGBA8Snorm, kR16Snorm, kRG16Snorm, kRGBA16Snorm, kBC5Snorm };
enum class Stage { kVertex, kFragment };
enum class Load { kDiscard, kLoad, kClear };
enum class Store { kDiscard, kStore };
enum class Primitive { kPoints, kLines, kLineStrip, kTriangles, kTriangleStrip };
enum class Compare { kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum class BlendFactor { kZero, kOne, kSourceAlpha, kOneMinusSourceAlpha, kSourceColor, kOneMinusSourceColor, kDestinationColor, kOneMinusDestinationColor, kDestinationAlpha, kOneMinusDestinationAlpha, kConstantColor, kOneMinusConstantColor, kConstantAlpha, kOneMinusConstantAlpha, kSourceAlphaSaturated };
enum class BlendOp { kAdd, kSubtract, kMin, kMax, kReverseSubtract };
enum class StencilOp { kKeep, kZero, kReplace, kIncrementClamp, kDecrementClamp, kInvert, kIncrementWrap, kDecrementWrap };
enum class Filter { kNearest, kLinear };

struct Resource { virtual ~Resource() = default; };
struct Buffer : Resource { virtual size_t size() const = 0; };
struct Target : Resource { virtual uint32_t width() const = 0; virtual uint32_t height() const = 0; };
struct Pipeline : Resource {};

struct ShaderCode {
  std::vector<uint32_t> spirv;
  std::string entry = "main";
  Stage stage = Stage::kVertex;
  bool flip_y = false;
};
struct TargetDesc {
  uint32_t width = 0, height = 0;
  Format format = Format::kRGBA8;
  std::string label;
  bool array_texture = false;
  uint32_t mip_levels = 1;
  uint32_t array_length = 1;
  uint32_t depth = 1;
  bool is_3d = false;
  bool is_1d = false;
  bool is_cube = false;
};
struct TextureLevel {
  uint32_t width, height;
  std::vector<std::byte> bytes;
  // Array slices for 2D arrays, depth slices for 3D textures.
  uint32_t depth = 1;
};
struct PipelineDesc {
  ShaderCode vertex, fragment;
  Format color = Format::kRGBA8;
  uint32_t raster_samples = 1;
  bool depth_stencil = false;
  bool depth_write = false;
  Compare depth_compare = Compare::kAlways;
  bool stencil = false;
  Compare stencil_compare = Compare::kAlways;
  StencilOp stencil_pass = StencilOp::kKeep;
  uint32_t stencil_read_mask = 255, stencil_write_mask = 255;
  bool blend = false;
  BlendFactor source_rgb = BlendFactor::kOne, destination_rgb = BlendFactor::kZero;
  BlendFactor source_alpha = BlendFactor::kOne, destination_alpha = BlendFactor::kZero;
  BlendOp rgb_op = BlendOp::kAdd, alpha_op = BlendOp::kAdd;
  uint8_t color_write_mask = 15;  // bits 0..3 = R, G, B, A
  std::string label;
};
struct PassDesc {
  // Nonzero dimensions select a raster pass with no texture attachments.
  uint32_t attachmentless_width = 0, attachmentless_height = 0, raster_samples = 1;
  std::vector<std::array<float,2>> sample_positions;
  // Null color means the current SDL drawable. Depth is always explicit.
  std::shared_ptr<Target> color, depth_stencil;
  Load color_load = Load::kClear, depth_load = Load::kClear, stencil_load = Load::kClear;
  Store color_store = Store::kStore, depth_store = Store::kStore, stencil_store = Store::kStore;
  std::array<double, 4> clear_color{0, 0, 0, 1};
  double clear_depth = 1;
  uint32_t clear_stencil = 0;
  std::string label;
};
struct BufferBinding {
  Stage stage;
  uint32_t descriptor_set, binding;
  std::shared_ptr<Buffer> buffer;
  size_t offset = 0;
};
struct TextureBinding {
  Stage stage;
  uint32_t descriptor_set, binding;
  std::shared_ptr<Target> texture;
};
enum class AddressMode { kRepeat, kMirrorRepeat, kClampEdge, kMirrorClampEdge, kClampBorder };
struct SamplerDesc {
  Filter min_filter = Filter::kNearest, mag_filter = Filter::kNearest;
  // 0 nearest, 1 linear, 2 base-only.
  uint32_t mip_filter = 2, min_lod = 0, max_lod = 0, anisotropy = 1;
  AddressMode u = AddressMode::kClampEdge, v = AddressMode::kClampEdge, w = AddressMode::kClampEdge;
  uint32_t border = 0;
};
struct SamplerBinding {
  Stage stage;
  uint32_t descriptor_set, binding;
  Filter filter = Filter::kLinear;
  bool repeat = false;
  std::optional<SamplerDesc> state;
};
struct Rectangle { uint32_t x = 0, y = 0, width = 0, height = 0; };
struct DrawDesc {
  std::shared_ptr<Pipeline> pipeline;
  std::vector<BufferBinding> buffers;
  std::vector<TextureBinding> textures;
  std::vector<SamplerBinding> samplers;
  Primitive primitive = Primitive::kTriangles;
  uint32_t first = 0, count = 0;
  // Optional index buffer; first is an index offset, not a vertex offset.
  std::shared_ptr<Buffer> indices;
  bool indices32 = false;
  int32_t base_vertex = 0;
  uint32_t stencil_reference = 0;
  // A zero width/height means use the complete render target.
  Rectangle viewport, scissor;
  double depth_min = 0, depth_max = 1;
  bool cull_front = false, cull_back = false, front_clockwise = false;
  std::array<float, 4> blend_constant{};
};
struct BackendConfig {
  // SDL calls and presentation are owned by the caller's main thread.
  SDL_Window* window = nullptr;
  std::filesystem::path cache_directory;
  bool vsync = true;
};
struct BackendStats {
  uint64_t submissions = 0, draws = 0, pipelines_created = 0;
  bool archive_loaded = false;
  // Accumulated on the rendering thread; read for the per-swap diagnostic.
  uint64_t residency_commit_us = 0, queue_submit_us = 0, wait_us = 0;
  uint64_t residency_additions = 0;
  uint64_t resolve_dispatches = 0, resolve_dispatch_us = 0;
  uint64_t resolve_commit_us = 0, resolve_wait_us = 0;
};
struct EdramResolveParams {
  uint32_t width = 0, height = 0;
  uint32_t src_base_tiles = 0, src_pitch_tiles = 0;
  uint32_t dest_base = 0, dest_pitch = 0;
  uint32_t src_offset_x = 0, src_offset_y = 0;
  uint32_t dest_offset_x = 0, dest_offset_y = 0;
  uint32_t endian_xor = 0, swap_rb = 0;
  uint32_t dest_size = 512u * 1024u * 1024u;
  uint32_t sample_count = 1, first_sample = 0;
  uint32_t x_shift = 0, y_shift = 0;
  uint32_t is_depth = 0;
};

// Initial host seam: deliberately small, extended only as guest paths are
// connected. A handle is valid only on the backend instance that created it.
// All calls except PreparePipeline are serialized by the rendering thread.
class HostBackend {
 public:
  virtual ~HostBackend() = default;
  virtual std::string device_name() const = 0;
  virtual std::shared_ptr<Buffer> CreateBuffer(std::span<const std::byte> bytes, const std::string& label) = 0;
  virtual std::shared_ptr<Buffer> CreateBufferSized(size_t size, const std::string& label) = 0;
  // Caller must wait for earlier users before overwriting shared storage.
  virtual void UpdateBuffer(const std::shared_ptr<Buffer>&, size_t offset, std::span<const std::byte>) = 0;
  // GPU copy resolve for the bitwise 32bpp path. The CPU resolver remains the
  // oracle and fallback for formats, clears and sample expansion.
  virtual void ResolveEdram(const std::shared_ptr<Buffer>&, const std::shared_ptr<Buffer>&,
                            const EdramResolveParams&) {
    throw std::runtime_error("EDRAM resolve is not implemented by this host backend");
  }
  virtual std::vector<std::byte> ReadBuffer(const std::shared_ptr<Buffer>&) = 0;
  virtual std::vector<std::byte> ReadBufferRange(const std::shared_ptr<Buffer>&, size_t, size_t) = 0;
  // Direct shared-buffer copy into a caller destination, without allocating a
  // full intermediate vector (used for the resolved-range publish).
  virtual void CopyBufferToHost(const std::shared_ptr<Buffer>&, size_t, size_t, void*) = 0;
  virtual std::shared_ptr<Target> CreateTarget(const TargetDesc&) = 0;
  virtual std::shared_ptr<Target> CreateTextureRGBA8(uint32_t width, uint32_t height,
      std::span<const std::byte> bytes, const std::string& label, bool array_texture = false) = 0;
  virtual std::shared_ptr<Target> CreateTexture2D(const TargetDesc&, std::span<const TextureLevel>) = 0;
  // UI-thread surface lifetime: detach before destroying the borrowed surface.
  // The Metal implementation accepts the runtime SDL surface's CAMetalLayer.
  virtual void AttachSurface(const void* surface, uint32_t width, uint32_t height) = 0;
  virtual void DetachSurface() = 0;
  // Call at load time; do not wait on this future inside a guest draw.
  virtual std::shared_future<std::shared_ptr<Pipeline>> PreparePipeline(PipelineDesc) = 0;
  // Offscreen submissions need no drawable; acquire one only for presentation.
  virtual bool BeginFrame(bool present = true) = 0;  // false if no drawable is available
  virtual void BeginPass(const PassDesc&) = 0;
  virtual void Draw(const DrawDesc&) = 0;
  virtual void EndPass() = 0;
  virtual void Submit() = 0;
  virtual void WaitIdle() = 0;
  // Diagnostic readback of single-sample RGBA8/BGRA8 targets, returned as RGBA8.
  virtual std::vector<uint8_t> Readback(const std::shared_ptr<Target>&) = 0;
  virtual void FlushPipelineCache() = 0;
  virtual BackendStats stats() const = 0;
};

std::unique_ptr<HostBackend> CreateMetal4Backend(const BackendConfig&);

}  // namespace mgr::gpu
