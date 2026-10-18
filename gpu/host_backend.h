#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

struct SDL_Window;

namespace mgr::gpu {

// Host operations only. PM4, registers, Xenos formats, endian conversion,
// primitive expansion, EDRAM ownership and guest completion stay in Xenos.
enum class Format { kRGBA8, kBGRA8, kRGBA16Float, kDepth32Stencil8 };
enum class Stage { kVertex, kFragment };
enum class Load { kDiscard, kLoad, kClear };
enum class Store { kDiscard, kStore };
enum class Primitive { kPoints, kLines, kLineStrip, kTriangles, kTriangleStrip };
enum class Compare { kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways };
enum class BlendFactor { kZero, kOne, kSourceAlpha, kOneMinusSourceAlpha };
enum class StencilOp { kKeep, kZero, kReplace, kIncrementClamp, kDecrementClamp, kInvert, kIncrementWrap, kDecrementWrap };

struct Resource { virtual ~Resource() = default; };
struct Buffer : Resource { virtual size_t size() const = 0; };
struct Target : Resource { virtual uint32_t width() const = 0; virtual uint32_t height() const = 0; };
struct Pipeline : Resource {};

struct ShaderCode {
  std::vector<uint32_t> spirv;
  std::string entry = "main";
  Stage stage = Stage::kVertex;
};
struct TargetDesc {
  uint32_t width = 0, height = 0;
  Format format = Format::kRGBA8;
  std::string label;
};
struct PipelineDesc {
  ShaderCode vertex, fragment;
  Format color = Format::kRGBA8;
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
  uint8_t color_write_mask = 15;  // bits 0..3 = R, G, B, A
  std::string label;
};
struct PassDesc {
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
struct DrawDesc {
  std::shared_ptr<Pipeline> pipeline;
  std::vector<BufferBinding> buffers;
  Primitive primitive = Primitive::kTriangles;
  uint32_t first = 0, count = 0;
  // Optional index buffer; first is an index offset, not a vertex offset.
  std::shared_ptr<Buffer> indices;
  bool indices32 = false;
  uint32_t stencil_reference = 0;
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
};

// Initial host seam: deliberately small, extended only as guest paths are
// connected. A handle is valid only on the backend instance that created it.
// All calls except PreparePipeline are serialized by the rendering thread.
class HostBackend {
 public:
  virtual ~HostBackend() = default;
  virtual std::string device_name() const = 0;
  virtual std::shared_ptr<Buffer> CreateBuffer(std::span<const std::byte> bytes, const std::string& label) = 0;
  virtual std::shared_ptr<Target> CreateTarget(const TargetDesc&) = 0;
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
