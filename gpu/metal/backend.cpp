#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <CommonCrypto/CommonDigest.h>

#include "host_backend.h"
#include "spirv_to_msl.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
const char kResolveEdramMsl[] = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct ResolveParams {
  uint width, height;
  uint src_base_tiles, src_pitch_tiles;
  uint dest_base, dest_pitch;
  uint src_offset_x, src_offset_y;
  uint dest_offset_x, dest_offset_y;
  uint endian_xor, swap_rb, dest_size;
  uint sample_count, first_sample, x_shift, y_shift;
  uint is_depth;
};
static inline uint tiled2d(uint x, uint y, uint pitch, uint bpe_log2) {
  pitch = (pitch + 31u) & ~31u;
  int macro = int((x >> 5) + (y >> 5) * (pitch >> 5)) << (bpe_log2 + 7);
  int micro = int((x & 7u) + ((y & 0xEu) << 2)) << bpe_log2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + int((y & 1u) << 4);
  return uint(((offset & ~0x1FF) << 3) + int((y & 16u) << 7) + ((offset & 0x1C0) << 2) +
      int(((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6) + (offset & 0x3F));
}
kernel void resolve_edram(device const uint* edram [[buffer(0)]],
                          device uchar* guest [[buffer(1)]],
                          constant ResolveParams& p [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= p.width || gid.y >= p.height) return;
  uint sum[4] = {0u, 0u, 0u, 0u};
  for (uint s = 0u; s < p.sample_count; ++s) {
    uint sample = p.first_sample + s;
    uint sx = ((gid.x + p.src_offset_x) << p.x_shift) + (sample >> 1);
    uint sy = ((gid.y + p.src_offset_y) << p.y_shift) + (sample & 1u);
    uint column = sx % 80u;
    // EDRAM depth tiles swap the two 40-column halves.
    if (p.is_depth != 0u) column = column < 40u ? column + 40u : column - 40u;
    uint tile = (p.src_base_tiles + (sy >> 4) * p.src_pitch_tiles + sx / 80u) & 2047u;
    uint src = tile * 5120u + ((sy & 15u) * 80u + column) * 4u;
    uchar4 bytes = as_type<uchar4>(edram[src >> 2]);
    sum[0] += bytes.x; sum[1] += bytes.y; sum[2] += bytes.z; sum[3] += bytes.w;
  }
  uchar out[4];
  if (p.sample_count == 1u) {
    out[0] = uchar(sum[0]); out[1] = uchar(sum[1]);
    out[2] = uchar(sum[2]); out[3] = uchar(sum[3]);
  } else {
    for (uint i = 0u; i < 4u; ++i) out[i] = uchar(rint(float(sum[i]) / float(p.sample_count)));
  }
  if (p.swap_rb != 0) { uchar t = out[0]; out[0] = out[2]; out[2] = t; }
  uint dx = gid.x + p.dest_offset_x;
  uint dy = gid.y + p.dest_offset_y;
  uint d = p.dest_base + tiled2d(dx, dy, p.dest_pitch, 2u);
  if (d + 4u > p.dest_size) return;
  guest[d + (0u ^ p.endian_xor)] = out[0];
  guest[d + (1u ^ p.endian_xor)] = out[1];
  guest[d + (2u ^ p.endian_xor)] = out[2];
  guest[d + (3u ^ p.endian_xor)] = out[3];
}
)METAL";
}
namespace mgr::gpu {
void ConfigureMetalLayer(void* layer);
void* CompileMetalPipeline(void* compiler, void* descriptor, void* options,
                          bool* archive_rejected, std::string* message);
namespace {
NS::String* Str(const std::string& s) { return NS::String::string(s.c_str(), NS::UTF8StringEncoding); }
std::string Error(NS::Error* e) { return e ? e->localizedDescription()->utf8String() : "no error detail"; }
void Require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
NS::URL* FileUrl(const std::filesystem::path& p) { return NS::URL::fileURLWithPath(Str(p.string())); }
std::string Digest(const std::string& data) {
  unsigned char hash[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), hash);
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto b : hash) { result += hex[b >> 4]; result += hex[b & 15]; }
  return result;
}
std::string FileDigest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  CC_SHA256_CTX context; CC_SHA256_Init(&context);
  std::array<char, 65536> block;
  while (input.read(block.data(), block.size()) || input.gcount())
    CC_SHA256_Update(&context, block.data(), static_cast<CC_LONG>(input.gcount()));
  if (!input.eof()) return {};
  unsigned char hash[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(hash, &context);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (auto b : hash) { result += hex[b >> 4]; result += hex[b & 15]; }
  return result;
}
std::string Key(const PipelineDesc& d) {
  std::string data = "mgr-host-pipeline-v1;spirv-cross=" MGR_SPIRV_CROSS_REV ";";
  auto number = [&](auto v) { data += std::to_string(static_cast<uint64_t>(v)) + ';'; };
  for (const auto* shader : {&d.vertex, &d.fragment}) {
    number(shader->stage); number(shader->flip_y); number(shader->entry.size()); data += shader->entry;
    number(shader->spirv.size());
    if (!shader->spirv.empty())
      data.append(reinterpret_cast<const char*>(shader->spirv.data()), shader->spirv.size() * 4);
  }
  number(d.color); number(d.raster_samples); number(d.depth_stencil); number(d.depth_write); number(d.depth_compare);
  number(d.stencil); number(d.stencil_compare); number(d.stencil_pass);
  number(d.stencil_read_mask); number(d.stencil_write_mask);
  number(d.blend); number(d.source_rgb); number(d.destination_rgb);
  number(d.source_alpha); number(d.destination_alpha); number(d.color_write_mask); number(d.rgb_op); number(d.alpha_op);
  return Digest(data);
}
MTL::PixelFormat PixelFormat(Format f) {
  switch (f) {
    case Format::kNone: return MTL::PixelFormatInvalid;
    case Format::kRGBA8: return MTL::PixelFormatRGBA8Unorm;
    case Format::kBGRA8: return MTL::PixelFormatBGRA8Unorm;
    case Format::kRGBA16Float: return MTL::PixelFormatRGBA16Float;
    case Format::kDepth32Stencil8: return MTL::PixelFormatDepth32Float_Stencil8;
    case Format::kR8: return MTL::PixelFormatR8Unorm;
    case Format::kRG8: return MTL::PixelFormatRG8Unorm;
    case Format::kR16: return MTL::PixelFormatR16Unorm;
    case Format::kRG16: return MTL::PixelFormatRG16Unorm;
    case Format::kRGBA16: return MTL::PixelFormatRGBA16Unorm;
    case Format::kR16Float: return MTL::PixelFormatR16Float;
    case Format::kRG16Float: return MTL::PixelFormatRG16Float;
    case Format::kR32Float: return MTL::PixelFormatR32Float;
    case Format::kRG32Float: return MTL::PixelFormatRG32Float;
    case Format::kRGBA32Float: return MTL::PixelFormatRGBA32Float;
    case Format::kBC1: return MTL::PixelFormatBC1_RGBA;
    case Format::kBC2: return MTL::PixelFormatBC2_RGBA;
    case Format::kBC3: return MTL::PixelFormatBC3_RGBA;
    case Format::kBC5: return MTL::PixelFormatBC5_RGUnorm;
    case Format::kR8Snorm: return MTL::PixelFormatR8Snorm;
    case Format::kRG8Snorm: return MTL::PixelFormatRG8Snorm;
    case Format::kRGBA8Snorm: return MTL::PixelFormatRGBA8Snorm;
    case Format::kR16Snorm: return MTL::PixelFormatR16Snorm;
    case Format::kRG16Snorm: return MTL::PixelFormatRG16Snorm;
    case Format::kRGBA16Snorm: return MTL::PixelFormatRGBA16Snorm;
    case Format::kBC5Snorm: return MTL::PixelFormatBC5_RGSnorm;
  }
  throw std::runtime_error("Unknown host pixel format");
}
MTL::BlendFactor Blend(BlendFactor f) {
  switch (f) {
    case BlendFactor::kZero: return MTL::BlendFactorZero;
    case BlendFactor::kOne: return MTL::BlendFactorOne;
    case BlendFactor::kSourceAlpha: return MTL::BlendFactorSourceAlpha;
    case BlendFactor::kOneMinusSourceAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
    case BlendFactor::kSourceColor: return MTL::BlendFactorSourceColor;
    case BlendFactor::kOneMinusSourceColor: return MTL::BlendFactorOneMinusSourceColor;
    case BlendFactor::kDestinationColor: return MTL::BlendFactorDestinationColor;
    case BlendFactor::kOneMinusDestinationColor: return MTL::BlendFactorOneMinusDestinationColor;
    case BlendFactor::kDestinationAlpha: return MTL::BlendFactorDestinationAlpha;
    case BlendFactor::kOneMinusDestinationAlpha: return MTL::BlendFactorOneMinusDestinationAlpha;
    case BlendFactor::kConstantColor: return MTL::BlendFactorBlendColor;
    case BlendFactor::kOneMinusConstantColor: return MTL::BlendFactorOneMinusBlendColor;
    case BlendFactor::kConstantAlpha: return MTL::BlendFactorBlendAlpha;
    case BlendFactor::kOneMinusConstantAlpha: return MTL::BlendFactorOneMinusBlendAlpha;
    case BlendFactor::kSourceAlphaSaturated: return MTL::BlendFactorSourceAlphaSaturated;
  }
  throw std::runtime_error("Unknown blend factor");
}
MTL::LoadAction LoadAction(Load l) {
  switch (l) {
    case Load::kDiscard: return MTL::LoadActionDontCare;
    case Load::kLoad: return MTL::LoadActionLoad;
    case Load::kClear: return MTL::LoadActionClear;
  }
  throw std::runtime_error("Unknown load action");
}
MTL::StoreAction StoreAction(Store s) { return s == Store::kStore ? MTL::StoreActionStore : MTL::StoreActionDontCare; }

struct Residency {
  NS::SharedPtr<MTL::ResidencySet> set;
  std::mutex mutex;
  bool dirty = false;
  std::atomic<uint64_t> additions{0};
  void Add(MTL::Allocation* allocation) {
    std::lock_guard lock(mutex); set->addAllocation(allocation); dirty = true;
    additions.fetch_add(1, std::memory_order_relaxed);
  }
  void Remove(MTL::Allocation* allocation) {
    std::lock_guard lock(mutex); set->removeAllocation(allocation); dirty = true;
  }
  void Commit() {
    std::lock_guard lock(mutex);
    if (dirty) { set->commit(); dirty = false; }
  }
  ~Residency() { if (set) set->endResidency(); }
};
struct MetalBuffer final : Buffer {
  const void* owner;
  std::shared_ptr<Residency> residency;
  NS::SharedPtr<MTL::Buffer> buffer;
  size_t size() const override { return buffer->length(); }
  ~MetalBuffer() override { residency->Remove(buffer.get()); }
};
struct MetalTarget final : Target {
  const void* owner;
  std::shared_ptr<Residency> residency;
  NS::SharedPtr<MTL::Texture> texture;
  Format format;
  uint32_t width() const override { return static_cast<uint32_t>(texture->width()); }
  uint32_t height() const override { return static_cast<uint32_t>(texture->height()); }
  ~MetalTarget() override { residency->Remove(texture.get()); }
};
struct MetalPipeline final : Pipeline {
  const void* owner;
  PipelineDesc desc;
  MslShader vertex, fragment;
  NS::SharedPtr<MTL::RenderPipelineState> state;
  NS::SharedPtr<MTL::DepthStencilState> depth;
};
struct Frame {
  NS::SharedPtr<MTL4::CommandAllocator> allocator;
  NS::SharedPtr<MTL4::CommandBuffer> command;
  NS::SharedPtr<MTL4::ArgumentTable> vertex, fragment, compute;
  uint64_t completion = 0;
  // Metal 4 command buffers do not retain these. Drop references only after
  // this slot's shared-event value has completed, never merely after N frames.
  std::vector<std::shared_ptr<Resource>> retained;
};
struct SdlMetalView {
  SDL_MetalView value = nullptr;
  void Reset() { if (value) SDL_Metal_DestroyView(value); value = nullptr; }
  ~SdlMetalView() { Reset(); }
};

class MetalBackend final : public HostBackend {
 public:
  explicit MetalBackend(const BackendConfig& config);
  ~MetalBackend() override;
  std::string device_name() const override { return device_->name()->utf8String(); }
  std::shared_ptr<Buffer> CreateBuffer(std::span<const std::byte>, const std::string&) override;
  std::shared_ptr<Buffer> CreateBufferSized(size_t, const std::string&) override;
  void UpdateBuffer(const std::shared_ptr<Buffer>&, size_t, std::span<const std::byte>) override;
  std::vector<std::byte> ReadBuffer(const std::shared_ptr<Buffer>&) override;
  std::vector<std::byte> ReadBufferRange(const std::shared_ptr<Buffer>&, size_t, size_t) override;
  void CopyBufferToHost(const std::shared_ptr<Buffer>&, size_t, size_t, void*) override;
  std::shared_ptr<Target> CreateTarget(const TargetDesc&) override;
  std::shared_ptr<Target> CreateTextureRGBA8(uint32_t, uint32_t, std::span<const std::byte>, const std::string&, bool = false) override;
  std::shared_ptr<Target> CreateTexture2D(const TargetDesc&, std::span<const TextureLevel>) override;
  void AttachSurface(const void*, uint32_t, uint32_t) override;
  void DetachSurface() override;
  std::shared_future<std::shared_ptr<Pipeline>> PreparePipeline(PipelineDesc) override;
  bool BeginFrame(bool present) override;
  void BeginPass(const PassDesc&) override;
  void Draw(const DrawDesc&) override;
  void EndPass() override;
  void Submit() override;
  void WaitIdle() override;
  std::vector<uint8_t> Readback(const std::shared_ptr<Target>&) override;
  void ResolveEdram(const std::shared_ptr<Buffer>&, const std::shared_ptr<Buffer>&,
                    const EdramResolveParams&) override;
  void FlushPipelineCache() override;
  BackendStats stats() const override {
    return {submissions_, draws_, pipelines_created_.load(), archive_loaded_,
        residency_commit_us_.load(), queue_submit_us_.load(), wait_us_.load(),
        residency_->additions.load(), resolve_dispatches_.load(),
        resolve_dispatch_us_.load(), resolve_commit_us_.load(), resolve_wait_us_.load(),
        pipeline_compile_us_.load(), pipeline_compiles_.load()};
  }

 private:
  template<class T, class U> std::shared_ptr<T> Cast(const std::shared_ptr<U>& r) {
    auto p = std::dynamic_pointer_cast<T>(r);
    Require(p && p->owner == this, "Resource belongs to a different backend");
    return p;
  }
  void Wait(uint64_t value);
  bool BeginCommands(bool acquire_drawable);
  std::shared_ptr<Pipeline> Compile(const PipelineDesc&);
  void EnsureResolvePipeline();
  void Worker();
  void Retain(const std::shared_ptr<Resource>& r) { frame_->retained.push_back(r); }
  BackendConfig config_;
  NS::SharedPtr<MTL::Device> device_;
  NS::SharedPtr<MTL4::CommandQueue> queue_;
  NS::SharedPtr<MTL::SharedEvent> event_;
  std::shared_ptr<Residency> residency_;
  SdlMetalView view_;
  CA::MetalLayer* layer_ = nullptr;  // owned by the SDL view, destroyed after GPU idle
  NS::SharedPtr<CA::MetalLayer> external_layer_;
  uint32_t surface_width_ = 0, surface_height_ = 0;
  NS::SharedPtr<MTL::SamplerState> samplers_[2][2];
  std::map<std::string, NS::SharedPtr<MTL::SamplerState>> guest_samplers_;
  CA::MetalDrawable* drawable_ = nullptr;  // lives in the current frame pool
  NS::SharedPtr<NS::AutoreleasePool> frame_pool_;
  std::array<Frame, 3> frames_;
  Frame* frame_ = nullptr;
  MTL4::RenderCommandEncoder* encoder_ = nullptr;
  Format pass_format_ = Format::kRGBA8;
  bool pass_depth_ = false;
  uint32_t width_ = 0, height_ = 0;
  uint64_t submissions_ = 0, draws_ = 0;
  std::atomic<uint64_t> pipelines_created_{0};
  std::atomic<uint64_t> residency_commit_us_{0}, queue_submit_us_{0}, wait_us_{0};
  std::atomic<uint64_t> resolve_dispatches_{0}, resolve_dispatch_us_{0};
  std::atomic<uint64_t> resolve_commit_us_{0}, resolve_wait_us_{0};
  std::atomic<uint64_t> pipeline_compile_us_{0}, pipeline_compiles_{0};
  NS::SharedPtr<MTL::ComputePipelineState> resolve_pipeline_;
  std::shared_ptr<Buffer> resolve_params_;
  std::mutex jobs_mutex_, compiler_mutex_;
  std::condition_variable jobs_ready_;
  std::deque<std::packaged_task<std::shared_ptr<Pipeline>()>> jobs_;
  std::map<std::string, std::shared_future<std::shared_ptr<Pipeline>>> pipelines_;
  bool stopping_ = false;
  std::thread worker_;
  NS::SharedPtr<MTL4::Compiler> compiler_;
  NS::SharedPtr<MTL4::PipelineDataSetSerializer> serializer_;
  NS::SharedPtr<MTL4::Archive> archive_;
  std::filesystem::path archive_path_;
  bool archive_loaded_ = false;
  bool cache_dirty_ = false;
};

MetalBackend::MetalBackend(const BackendConfig& config) : config_(config) {
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  device_ = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
  Require(device_ && device_->supportsFamily(MTL::GPUFamilyMetal4) && device_->supportsFamily(MTL::GPUFamilyApple7),
          "Metal 4 on Apple Silicon (M1 or newer, macOS 26+) is required");
  auto qd = NS::TransferPtr(MTL4::CommandQueueDescriptor::alloc()->init());
  qd->setLabel(Str("MGR native host queue"));
  NS::Error* error = nullptr;
  queue_ = NS::TransferPtr(device_->newMTL4CommandQueue(qd.get(), &error));
  Require(bool(queue_), "Create Metal 4 queue: " + Error(error));
  event_ = NS::TransferPtr(device_->newSharedEvent());
  Require(bool(event_), "Create GPU completion event");
  event_->setLabel(Str("MGR submission completion"));
  auto rd = NS::TransferPtr(MTL::ResidencySetDescriptor::alloc()->init());
  rd->setLabel(Str("MGR persistent resources")); rd->setInitialCapacity(128);
  residency_ = std::make_shared<Residency>();
  residency_->set = NS::TransferPtr(device_->newResidencySet(rd.get(), &error));
  Require(bool(residency_->set), "Create residency set: " + Error(error));
  residency_->set->commit(); residency_->set->requestResidency();
  queue_->addResidencySet(residency_->set.get());
  for (size_t i = 0; i < frames_.size(); ++i) {
    auto& f = frames_[i];
    auto allocator_desc = NS::TransferPtr(MTL4::CommandAllocatorDescriptor::alloc()->init());
    allocator_desc->setLabel(Str("MGR frame allocator " + std::to_string(i)));
    f.allocator = NS::TransferPtr(device_->newCommandAllocator(allocator_desc.get(), &error));
    f.command = NS::TransferPtr(device_->newCommandBuffer());
    Require(f.allocator && f.command, "Create command slot");
    f.command->setLabel(Str("MGR frame commands " + std::to_string(i)));
    auto ad = NS::TransferPtr(MTL4::ArgumentTableDescriptor::alloc()->init());
    ad->setMaxBufferBindCount(31); ad->setInitializeBindings(true);
    ad->setMaxTextureBindCount(128); ad->setMaxSamplerStateBindCount(16);
    f.vertex = NS::TransferPtr(device_->newArgumentTable(ad.get(), &error));
    f.fragment = NS::TransferPtr(device_->newArgumentTable(ad.get(), &error));
    auto cad = NS::TransferPtr(MTL4::ArgumentTableDescriptor::alloc()->init());
    cad->setMaxBufferBindCount(8); cad->setMaxTextureBindCount(8); cad->setMaxSamplerStateBindCount(8);
    cad->setInitializeBindings(true);
    f.compute = NS::TransferPtr(device_->newArgumentTable(cad.get(), &error));
    Require(f.vertex && f.fragment && f.compute, "Create argument tables: " + Error(error));
  }
  for (int filter = 0; filter < 2; ++filter) for (int repeat = 0; repeat < 2; ++repeat) {
    auto sd = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
    sd->setMinFilter(filter ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
    sd->setMagFilter(filter ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
    sd->setSAddressMode(repeat ? MTL::SamplerAddressModeRepeat : MTL::SamplerAddressModeClampToEdge);
    sd->setTAddressMode(repeat ? MTL::SamplerAddressModeRepeat : MTL::SamplerAddressModeClampToEdge);
    sd->setSupportArgumentBuffers(true);
    sd->setLabel(Str("MGR native sampler " + std::to_string(filter) + ":" + std::to_string(repeat)));
    samplers_[filter][repeat] = NS::TransferPtr(device_->newSamplerState(sd.get()));
    Require(bool(samplers_[filter][repeat]), "Create native sampler");
  }
  Require(device_->areRasterOrderGroupsSupported(), "Native Xenos rendering requires raster order groups");
  auto sd = NS::TransferPtr(MTL4::PipelineDataSetSerializerDescriptor::alloc()->init());
  // Capture binaries alone: the combined descriptor+binary mode returned
  // false without an NSError on the tested M2 driver. Binary-only archives
  // round-trip successfully. Recipe capture is a separate diagnostic task.
  sd->setConfiguration(MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
  serializer_ = NS::TransferPtr(device_->newPipelineDataSetSerializer(sd.get()));
  Require(bool(serializer_), "Create pipeline serializer");
  auto cd = NS::TransferPtr(MTL4::CompilerDescriptor::alloc()->init());
  cd->setLabel(Str("MGR pipeline worker compiler"));
  cd->setPipelineDataSetSerializer(serializer_.get());
  compiler_ = NS::TransferPtr(device_->newCompiler(cd.get(), &error));
  Require(bool(compiler_), "Create Metal compiler: " + Error(error));
  const char* validation = std::getenv("MTL_SHADER_VALIDATION");
  // Instrumented archives from this driver may serialize successfully yet be
  // rejected as an incompatible deployment target on the next launch. Never
  // mix them with normal binaries, or persist them as a working warm cache.
  const bool instrumented = validation && std::strcmp(validation, "0") != 0;
  if (!config.cache_directory.empty() && !instrumented) {
    // GPU and OS identity isolate incompatible binaries. Shader and complete
    // pipeline descriptions remain the compiler's own archive lookup keys.
    auto identity = std::to_string(device_->registryID()) + ':' +
        NS::ProcessInfo::processInfo()->operatingSystemVersionString()->utf8String();
    for (const char* variable : {"MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION",
         "MTL_SHADER_VALIDATION_ENABLE_PIPELINES", "MTL_SHADER_VALIDATION_DISABLE_PIPELINES"}) {
      const char* value = std::getenv(variable);
      identity += std::string(":") + variable + '=' + (value ? value : "");
    }
    auto dir = config.cache_directory / ("metal4-v1-" + Digest(identity).substr(0, 16));
    std::filesystem::create_directories(dir);
    archive_path_ = dir / "pipelines.metallib";
    if (std::filesystem::exists(archive_path_)) {
      // Some drivers crash, rather than returning NSError, on truncated
      // archives. Only hand intact files written by this cache to Metal.
      std::ifstream checksum(archive_path_.string() + ".sha256");
      std::string expected; std::getline(checksum, expected);
      if (expected.size() == 64 && expected == FileDigest(archive_path_)) {
        archive_ = NS::TransferPtr(device_->newArchive(FileUrl(archive_path_), &error));
        archive_loaded_ = bool(archive_);
        if (!archive_) std::fprintf(stderr, "[metal4] ignoring incompatible archive: %s\n", Error(error).c_str());
      } else std::fprintf(stderr, "[metal4] ignoring incomplete or damaged archive\n");
    }
  }
  if (config.window) {
    view_.value = SDL_Metal_CreateView(config.window);
    Require(view_.value, std::string("SDL Metal view: ") + SDL_GetError());
    layer_ = static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(view_.value));
    Require(layer_, "SDL returned no Metal layer");
    layer_->setDevice(device_.get()); layer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    layer_->setFramebufferOnly(true); ConfigureMetalLayer(layer_);
    layer_->setMaximumDrawableCount(3); layer_->setDisplaySyncEnabled(config.vsync);
    queue_->addResidencySet(layer_->residencySet());
  }
  worker_ = std::thread([this] { Worker(); });
}

MetalBackend::~MetalBackend() {
  { std::lock_guard lock(jobs_mutex_); stopping_ = true; }
  jobs_ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  try {
    if (encoder_) EndPass();
    if (frame_) Submit();
    WaitIdle();
    FlushPipelineCache();
  } catch (const std::exception& e) {
    // Releasing possibly in-flight resources after a failed completion wait
    // would turn a diagnosed GPU failure into a use-after-free.
    std::fprintf(stderr, "[metal4] fatal shutdown failure: %s\n", e.what());
    std::terminate();
  }
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  for (auto& f : frames_) f.retained.clear();
  residency_->Commit();
  queue_->removeResidencySet(residency_->set.get());
  if (layer_) queue_->removeResidencySet(layer_->residencySet());
  view_.Reset();
}

std::shared_ptr<Buffer> MetalBackend::CreateBuffer(std::span<const std::byte> bytes, const std::string& label) {
  Require(!bytes.empty() && bytes.size() <= device_->maxBufferLength(), "Invalid buffer length");
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto handle = NS::TransferPtr(device_->newBuffer(bytes.data(), bytes.size(), MTL::ResourceStorageModeShared));
  Require(bool(handle), "Buffer allocation failed"); handle->setLabel(Str(label));
  auto result = std::make_shared<MetalBuffer>();
  result->owner = this; result->residency = residency_; result->buffer = std::move(handle);
  residency_->Add(result->buffer.get()); return result;
}

std::shared_ptr<Buffer> MetalBackend::CreateBufferSized(size_t size, const std::string& label) {
  Require(size && size <= device_->maxBufferLength(), "Invalid buffer length");
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto result = std::make_shared<MetalBuffer>();
  result->owner = this; result->residency = residency_;
  result->buffer = NS::TransferPtr(device_->newBuffer(size, MTL::ResourceStorageModeShared));
  Require(bool(result->buffer), "Buffer allocation failed");
  result->buffer->setLabel(Str(label)); residency_->Add(result->buffer.get()); return result;
}
void MetalBackend::UpdateBuffer(const std::shared_ptr<Buffer>& buffer, size_t offset, std::span<const std::byte> bytes) {
  auto native = Cast<MetalBuffer>(buffer);
  Require(offset <= native->size() && bytes.size() <= native->size() - offset, "Buffer update out of bounds");
  std::memcpy(static_cast<std::byte*>(native->buffer->contents()) + offset, bytes.data(), bytes.size());
}

std::vector<std::byte> MetalBackend::ReadBuffer(const std::shared_ptr<Buffer>& buffer) {
  return ReadBufferRange(buffer, 0, Cast<MetalBuffer>(buffer)->size());
}
std::vector<std::byte> MetalBackend::ReadBufferRange(const std::shared_ptr<Buffer>& buffer,
                                                     size_t offset, size_t size) {
  WaitIdle(); auto native = Cast<MetalBuffer>(buffer);
  Require(offset <= native->size() && size <= native->size() - offset, "Buffer read range out of bounds");
  const auto* data = static_cast<const std::byte*>(native->buffer->contents()) + offset;
  return {data, data + size};
}
void MetalBackend::CopyBufferToHost(const std::shared_ptr<Buffer>& buffer, size_t offset,
                                    size_t size, void* destination) {
  WaitIdle(); auto native = Cast<MetalBuffer>(buffer);
  Require(offset <= native->size() && size <= native->size() - offset, "Buffer read range out of bounds");
  std::memcpy(destination, static_cast<const std::byte*>(native->buffer->contents()) + offset, size);
}

std::shared_ptr<Target> MetalBackend::CreateTarget(const TargetDesc& desc) {
  Require(desc.format != Format::kNone && desc.width && desc.height, "Render target dimensions must be nonzero");
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto td = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
  td->setTextureType(desc.array_texture ? MTL::TextureType2DArray : MTL::TextureType2D); td->setArrayLength(1); td->setWidth(desc.width); td->setHeight(desc.height);
  td->setPixelFormat(PixelFormat(desc.format)); td->setStorageMode(MTL::StorageModePrivate);
  td->setUsage(desc.format == Format::kDepth32Stencil8 ? MTL::TextureUsageRenderTarget :
      MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
  auto handle = NS::TransferPtr(device_->newTexture(td.get()));
  Require(bool(handle), "Render target allocation failed: " + desc.label); handle->setLabel(Str(desc.label));
  auto result = std::make_shared<MetalTarget>();
  result->owner = this; result->residency = residency_; result->texture = std::move(handle); result->format = desc.format;
  residency_->Add(result->texture.get()); return result;
}

std::shared_ptr<Target> MetalBackend::CreateTextureRGBA8(uint32_t width, uint32_t height,
    std::span<const std::byte> bytes, const std::string& label, bool array_texture) {
  Require(!frame_ && width && height && uint64_t(width) * height * 4 == bytes.size(), "Invalid texture upload");
  auto target = Cast<MetalTarget>(CreateTarget({width, height, Format::kRGBA8, label, array_texture}));
  const size_t pitch = (size_t(width) * 4 + 255) & ~size_t(255);
  std::vector<std::byte> padded(pitch * height);
  for (uint32_t y = 0; y < height; ++y)
    std::memcpy(padded.data() + y * pitch, bytes.data() + size_t(y) * width * 4, size_t(width) * 4);
  auto upload = Cast<MetalBuffer>(CreateBuffer(padded, label + " upload"));
  BeginCommands(false); Retain(target); Retain(upload);
  auto* encoder = frame_->command->computeCommandEncoder();
  Require(encoder, "Create texture upload encoder"); encoder->setLabel(Str(label + " upload"));
  encoder->barrierAfterQueueStages(MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice);
  encoder->copyFromBuffer(upload->buffer.get(), 0, pitch, pitch * height,
      MTL::Size(width, height, 1), target->texture.get(), 0, 0, MTL::Origin(0, 0, 0));
  encoder->barrierAfterStages(MTL::StageBlit, MTL::StageAll, MTL4::VisibilityOptionDevice);
  encoder->endEncoding(); Submit(); return target;
}
std::shared_ptr<Target> MetalBackend::CreateTexture2D(const TargetDesc& desc, std::span<const TextureLevel> levels) {
  Require(!frame_ && desc.width && desc.height && levels.size() == desc.mip_levels, "Invalid guest texture description");
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  const bool compressed = desc.format == Format::kBC1 || desc.format == Format::kBC2 || desc.format == Format::kBC3 || desc.format == Format::kBC5 || desc.format == Format::kBC5Snorm;
  Require(!compressed || device_->supportsBCTextureCompression(), "BC texture compression unavailable");
  auto td = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
  td->setTextureType(desc.is_3d ? MTL::TextureType3D :
      desc.is_1d ? MTL::TextureType1D :
      desc.is_cube ? MTL::TextureTypeCube :
      desc.array_texture ? MTL::TextureType2DArray : MTL::TextureType2D);
  td->setArrayLength(desc.is_3d || desc.is_1d || desc.is_cube ? 1 : desc.array_length);
  td->setDepth(desc.is_3d ? desc.depth : 1);
  td->setWidth(desc.width); td->setHeight(desc.height); td->setMipmapLevelCount(desc.mip_levels);
  td->setPixelFormat(PixelFormat(desc.format)); td->setStorageMode(MTL::StorageModeShared);
  td->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
  auto result = std::make_shared<MetalTarget>(); result->owner = this; result->residency = residency_; result->format = desc.format;
  result->texture = NS::TransferPtr(device_->newTexture(td.get())); Require(bool(result->texture), "Guest texture allocation failed");
  result->texture->setLabel(Str(desc.label)); residency_->Add(result->texture.get());
  for (uint32_t mip = 0; mip < levels.size(); ++mip) {
    const auto& level = levels[mip];
    Require(level.width == std::max(1u, desc.width >> mip) && level.height == std::max(1u, desc.height >> mip), "Invalid mip extent");
    const uint32_t slices = std::max(1u, level.depth);
    const size_t rows = compressed ? (level.height + 3) / 4 : level.height;
    Require(rows && !level.bytes.empty() && level.bytes.size() % (rows * slices) == 0, "Invalid texture level bytes");
    const size_t row_size = level.bytes.size() / (rows * slices), pitch = (row_size + 255) & ~size_t(255);
    std::vector<std::byte> padded(pitch * rows * slices);
    for (uint32_t s = 0; s < slices; ++s)
      for (size_t y = 0; y < rows; ++y)
        std::memcpy(padded.data() + (size_t(s) * rows + y) * pitch,
                    level.bytes.data() + (size_t(s) * rows + y) * row_size, row_size);
    auto upload = Cast<MetalBuffer>(CreateBuffer(padded, desc.label + " mip upload"));
    Require(BeginCommands(false), "Begin texture upload"); Retain(result); Retain(upload);
    auto* copy = frame_->command->computeCommandEncoder(); Require(copy, "Create texture upload encoder");
    copy->setLabel(Str(desc.label + " mip " + std::to_string(mip)));
    copy->barrierAfterQueueStages(MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice);
    for (uint32_t s = 0; s < slices; ++s)
      copy->copyFromBuffer(upload->buffer.get(), size_t(s) * pitch * rows, pitch, pitch * rows,
          MTL::Size(level.width, level.height, 1), result->texture.get(),
          desc.is_3d ? 0 : s, mip, MTL::Origin(0, 0, desc.is_3d ? s : 0));
    copy->barrierAfterStages(MTL::StageBlit, MTL::StageAll, MTL4::VisibilityOptionDevice);
    copy->endEncoding(); Submit();
  }
  return result;
}

void MetalBackend::AttachSurface(const void* surface, uint32_t width, uint32_t height) {
  Require(surface && width && height && !view_.value, "Invalid external surface");
  WaitIdle();
  auto* incoming = const_cast<CA::MetalLayer*>(static_cast<const CA::MetalLayer*>(surface));
  if (layer_ != incoming) {
    if (layer_) queue_->removeResidencySet(layer_->residencySet());
    external_layer_ = NS::RetainPtr(incoming); layer_ = incoming;
    layer_->setDevice(device_.get()); layer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
    layer_->setFramebufferOnly(true); ConfigureMetalLayer(layer_);
    layer_->setMaximumDrawableCount(3); layer_->setDisplaySyncEnabled(config_.vsync);
    queue_->addResidencySet(layer_->residencySet());
  }
  surface_width_ = width; surface_height_ = height;
  layer_->setDrawableSize(CGSizeMake(width, height));
}
void MetalBackend::DetachSurface() {
  Require(!view_.value, "SDL-owned smoke surface cannot be detached externally");
  WaitIdle();
  if (layer_) queue_->removeResidencySet(layer_->residencySet());
  layer_ = nullptr; external_layer_.reset(); surface_width_ = surface_height_ = 0;
}

std::shared_future<std::shared_ptr<Pipeline>> MetalBackend::PreparePipeline(PipelineDesc desc) {
  const auto key = Key(desc);
  std::lock_guard lock(jobs_mutex_);
  Require(!stopping_, "Pipeline compiler is shutting down");
  if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
  std::packaged_task<std::shared_ptr<Pipeline>()> task([this, desc = std::move(desc)] { return Compile(desc); });
  auto future = task.get_future().share();
  pipelines_.emplace(key, future); jobs_.push_back(std::move(task)); jobs_ready_.notify_one();
  return future;
}
void MetalBackend::Worker() {
  for (;;) {
    std::packaged_task<std::shared_ptr<Pipeline>()> task;
    { std::unique_lock lock(jobs_mutex_);
      jobs_ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;
      task = std::move(jobs_.front()); jobs_.pop_front(); }
    auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    task();  // packaged_task reports failures through its future
  }
}
std::shared_ptr<Pipeline> MetalBackend::Compile(const PipelineDesc& desc) {
  const auto compile_begin = std::chrono::steady_clock::now();
  Require(desc.vertex.stage == Stage::kVertex && desc.fragment.stage == Stage::kFragment,
          "Pipeline stages do not match vertex/fragment slots");
  Require(desc.color != Format::kDepth32Stencil8 && desc.color_write_mask <= 15, "Invalid color state");
  Require(!desc.stencil || desc.depth_stencil, "Stencil state requires an attachment");
  Require(desc.stencil_read_mask <= 255 && desc.stencil_write_mask <= 255, "Invalid stencil masks");
  Require(static_cast<unsigned>(desc.depth_compare) <= static_cast<unsigned>(Compare::kAlways) &&
          static_cast<unsigned>(desc.stencil_compare) <= static_cast<unsigned>(Compare::kAlways) &&
          static_cast<unsigned>(desc.stencil_pass) <= static_cast<unsigned>(StencilOp::kDecrementWrap), "Invalid depth/stencil state");
  std::lock_guard lock(compiler_mutex_);
  auto result = std::make_shared<MetalPipeline>(); result->owner = this; result->desc = desc;
  result->vertex = TranslateSpirvToMsl(desc.vertex); result->fragment = TranslateSpirvToMsl(desc.fragment);
  if (std::getenv("MGR_METAL4_FRAGMENT_COUNTERS") &&
      (desc.label.find("17894137481653627549") != std::string::npos ||
       desc.label.find("5619865268510285684") != std::string::npos)) {
    auto& source = result->fragment.source;
    const auto entry = source.find("fragment void ");
    Require(entry != std::string::npos, "Diagnostic requires a void fragment entry");
    const auto args_begin = source.find('(', entry);
    Require(args_begin != std::string::npos, "Cannot locate fragment arguments");
    size_t args_end = args_begin + 1;
    unsigned nesting = 1;
    for (; args_end < source.size() && nesting; ++args_end) {
      if (source[args_end] == '(') ++nesting;
      else if (source[args_end] == ')') --nesting;
    }
    Require(!nesting, "Unterminated fragment arguments");
    --args_end;
    const auto body = source.find('{', args_end);
    Require(args_end != std::string::npos && body != std::string::npos,
            "Cannot locate fragment entry for counters");
    source.insert(args_end, ", device atomic_uint* mgr_diag [[buffer(30)]]");
    const auto instrumented_body = source.find('{', entry);
    source.insert(instrumented_body + 1,
        "\n    atomic_fetch_add_explicit(&mgr_diag[0], 1u, memory_order_relaxed);\n");
    // The color-written branch begins output-merger processing after the
    // guest shader body; the earlier EDRAM accesses are depth-only branches.
    const auto merger = source.find("if ((xe_var_fsi_color_written & 1u) != 0u)", instrumented_body);
    Require(merger != std::string::npos, "Diagnostic output merger branch missing");
    const auto merger_line = source.rfind('\n', merger);
    source.insert(merger_line + 1,
        "    atomic_fetch_add_explicit(&mgr_diag[1], 1u, memory_order_relaxed);\n");
    size_t pos = source.find('{', entry);
    while ((pos = source.find("xe_edram.edram[", pos)) != std::string::npos) {
      const auto end = source.find(';', pos);
      if (end == std::string::npos) break;
      const auto equals = source.find('=', pos);
      if (equals != std::string::npos && equals < end &&
          (equals == 0 || source[equals - 1] != '=')) {
        const auto line = source.rfind('\n', pos);
        constexpr char counter[] =
            "    atomic_fetch_add_explicit(&mgr_diag[2], 1u, memory_order_relaxed);\n";
        source.insert(line + 1, counter);
        pos = source.find(';', pos + sizeof(counter) - 1) + 1;
      } else pos = end + 1;
    }
  }
  if (const char* dump = std::getenv("MGR_METAL4_DUMP_MSL"); dump && *dump &&
      desc.label.find(dump) != std::string::npos) {
    std::ofstream("/private/tmp/mgr-metal4-vertex.metal") << result->vertex.source;
    std::ofstream("/private/tmp/mgr-metal4-fragment.metal") << result->fragment.source;
  }
  Require(!result->vertex.needs_extended_bindings && !result->fragment.needs_extended_bindings,
          "Draw pipeline requires texture, sampler or push-constant bindings not connected yet");
  auto library = [&](const MslShader& msl) {
    auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
    options->setMathMode(MTL::MathModeSafe);
    options->setLanguageVersion(MTL::LanguageVersion3_2);
    auto ld = NS::TransferPtr(MTL4::LibraryDescriptor::alloc()->init());
    ld->setName(Str("mgr_" + Digest(msl.source)));
    ld->setSource(Str(msl.source)); ld->setOptions(options.get());
    NS::Error* error = nullptr;
    auto lib = NS::TransferPtr(compiler_->newLibrary(ld.get(), &error));
    Require(bool(lib), "MSL compilation: " + Error(error)); return lib;
  };
  auto vs = library(result->vertex), ps = library(result->fragment);
  auto vf = NS::TransferPtr(MTL4::LibraryFunctionDescriptor::alloc()->init());
  auto pf = NS::TransferPtr(MTL4::LibraryFunctionDescriptor::alloc()->init());
  vf->setLibrary(vs.get()); vf->setName(Str(result->vertex.entry));
  pf->setLibrary(ps.get()); pf->setName(Str(result->fragment.entry));
  auto pd = NS::TransferPtr(MTL4::RenderPipelineDescriptor::alloc()->init());
  pd->setLabel(Str(desc.label)); pd->setVertexFunctionDescriptor(vf.get()); pd->setFragmentFunctionDescriptor(pf.get());
  pd->setRasterizationEnabled(true);
  Require(pd->isRasterizationEnabled(), "Guest pipeline rasterization was disabled");
  Require(device_->supportsTextureSampleCount(desc.raster_samples), "Unsupported raster sample count");
  pd->setRasterSampleCount(desc.raster_samples);
  auto* color = pd->colorAttachments()->object(0);
  color->setPixelFormat(PixelFormat(desc.color));
  color->setBlendingState(desc.blend ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);
  color->setSourceRGBBlendFactor(Blend(desc.source_rgb)); color->setDestinationRGBBlendFactor(Blend(desc.destination_rgb));
  color->setSourceAlphaBlendFactor(Blend(desc.source_alpha)); color->setDestinationAlphaBlendFactor(Blend(desc.destination_alpha));
  const MTL::BlendOperation ops[] = {MTL::BlendOperationAdd, MTL::BlendOperationSubtract,
      MTL::BlendOperationMin, MTL::BlendOperationMax, MTL::BlendOperationReverseSubtract};
  Require(size_t(desc.rgb_op) < std::size(ops) && size_t(desc.alpha_op) < std::size(ops), "Invalid blend operation");
  color->setRgbBlendOperation(ops[size_t(desc.rgb_op)]); color->setAlphaBlendOperation(ops[size_t(desc.alpha_op)]);
  const auto mask = desc.color_write_mask;
  color->setWriteMask(static_cast<MTL::ColorWriteMask>(
      ((mask & 1) ? MTL::ColorWriteMaskRed : 0) | ((mask & 2) ? MTL::ColorWriteMaskGreen : 0) |
      ((mask & 4) ? MTL::ColorWriteMaskBlue : 0) | ((mask & 8) ? MTL::ColorWriteMaskAlpha : 0)));
  // Metal 4 infers depth/stencil attachment formats from the render pass.
  auto task_options = NS::TransferPtr(MTL4::CompilerTaskOptions::alloc()->init());
  if (archive_) { const NS::Object* archives[] = {archive_.get()};
    task_options->setLookupArchives(NS::Array::array(archives, 1)); }
  bool archive_rejected = false;
  std::string message;
  result->state = NS::TransferPtr(static_cast<MTL::RenderPipelineState*>(
      CompileMetalPipeline(compiler_.get(), pd.get(), task_options.get(), &archive_rejected, &message)));
  if (archive_rejected) {
    std::fprintf(stderr, "[metal4] archive rejected by compiler; rebuilt pipeline from shader source\n");
    archive_.reset();
  }
  Require(bool(result->state), "Pipeline compilation: " + message);
  auto depth = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
  depth->setLabel(Str(desc.label + " depth/stencil"));
  depth->setDepthWriteEnabled(desc.depth_write);
  depth->setDepthCompareFunction(static_cast<MTL::CompareFunction>(desc.depth_compare));
  if (desc.stencil) {
    auto stencil = NS::TransferPtr(MTL::StencilDescriptor::alloc()->init());
    stencil->setStencilCompareFunction(static_cast<MTL::CompareFunction>(desc.stencil_compare));
    stencil->setDepthStencilPassOperation(static_cast<MTL::StencilOperation>(desc.stencil_pass));
    stencil->setStencilFailureOperation(MTL::StencilOperationKeep);
    stencil->setDepthFailureOperation(MTL::StencilOperationKeep);
    stencil->setReadMask(desc.stencil_read_mask); stencil->setWriteMask(desc.stencil_write_mask);
    depth->setFrontFaceStencil(stencil.get()); depth->setBackFaceStencil(stencil.get());
  }
  result->depth = NS::TransferPtr(device_->newDepthStencilState(depth.get()));
  Require(bool(result->depth), "Depth state creation failed");
  ++pipelines_created_; cache_dirty_ = true;
  const auto compile_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - compile_begin).count());
  pipeline_compile_us_.fetch_add(compile_us, std::memory_order_relaxed);
  pipeline_compiles_.fetch_add(1, std::memory_order_relaxed);
  if (std::getenv("MGR_METAL4_TRACE_PIPELINES"))
    std::fprintf(stderr, "[metal4] pipeline '%s' compiled in %llu us\n",
                 desc.label.c_str(), static_cast<unsigned long long>(compile_us));
  return result;
}

void MetalBackend::Wait(uint64_t value) {
  if (!value) return;
  const auto begin = std::chrono::steady_clock::now();
  const bool signaled = event_->waitUntilSignaledValue(value, 30000);
  wait_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
  if (!signaled) throw std::runtime_error("GPU submission failed to complete within 30 seconds");
}
bool MetalBackend::BeginFrame(bool present) {
  return BeginCommands(present);
}
bool MetalBackend::BeginCommands(bool acquire_drawable) {
  Require(!frame_ && !encoder_, "Frame already open");
  frame_pool_ = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto& next = frames_[submissions_ % frames_.size()];
  Wait(next.completion); next.retained.clear();
  if (layer_ && acquire_drawable) {
    int w = surface_width_, h = surface_height_;
    if (config_.window) Require(SDL_GetWindowSizeInPixels(config_.window, &w, &h), SDL_GetError());
    if (w <= 0 || h <= 0 || (config_.window && (SDL_GetWindowFlags(config_.window) & SDL_WINDOW_MINIMIZED))) {
      frame_pool_.reset(); return false;
    }
    const auto size = layer_->drawableSize();
    if (size.width != w || size.height != h) {
      WaitIdle();  // resize between frames only, after old drawables are complete
      layer_->setDrawableSize(CGSizeMake(w, h));
    }
    drawable_ = layer_->nextDrawable();
    if (!drawable_) { frame_pool_.reset(); return false; }
  }
  next.allocator->reset(); next.command->beginCommandBuffer(next.allocator.get());
  frame_ = &next; return true;
}
void MetalBackend::BeginPass(const PassDesc& desc) {
  Require(frame_ && !encoder_, "Pass requires an open frame with no active encoder");
  MTL::Texture* color = nullptr;
  if (desc.attachmentless_width && desc.attachmentless_height) {
    Require(!desc.color && !desc.depth_stencil, "Attachmentless pass has textures");
    pass_format_ = Format::kNone;
  } else if (desc.color) {
    auto target = Cast<MetalTarget>(desc.color);
    Require(target->format != Format::kDepth32Stencil8, "Depth texture used as color");
    color = target->texture.get(); pass_format_ = target->format; Retain(target);
  } else {
    Require(drawable_, "No drawable for window pass");
    color = drawable_->texture(); pass_format_ = Format::kBGRA8;
  }
  width_ = color ? static_cast<uint32_t>(color->width()) : desc.attachmentless_width;
  height_ = color ? static_cast<uint32_t>(color->height()) : desc.attachmentless_height;
  auto rp = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
  rp->setRenderTargetWidth(width_); rp->setRenderTargetHeight(height_);
  rp->setDefaultRasterSampleCount(desc.raster_samples);
  Require(rp->renderTargetWidth() == width_ && rp->renderTargetHeight() == height_ &&
              rp->defaultRasterSampleCount() == desc.raster_samples,
          "Metal 4 render pass coverage dimensions or sample count were not applied");
  if (!desc.sample_positions.empty()) {
    Require(desc.sample_positions.size() == desc.raster_samples, "Sample position count mismatch");
    std::vector<MTL::SamplePosition> positions;
    for (const auto& xy : desc.sample_positions) positions.push_back(MTL::SamplePosition{xy[0], xy[1]});
    rp->setSamplePositions(positions.data(), positions.size());
  }
  if (color) {
  auto* ca = rp->colorAttachments()->object(0);
  ca->setTexture(color); ca->setLoadAction(LoadAction(desc.color_load)); ca->setStoreAction(StoreAction(desc.color_store));
  const auto& c = desc.clear_color; ca->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
  }
  pass_depth_ = bool(desc.depth_stencil);
  if (pass_depth_) {
    auto target = Cast<MetalTarget>(desc.depth_stencil);
    Require(target->format == Format::kDepth32Stencil8 && target->width() == width_ && target->height() == height_,
            "Depth target format or size mismatch"); Retain(target);
    auto* da = rp->depthAttachment(); da->setTexture(target->texture.get());
    da->setLoadAction(LoadAction(desc.depth_load)); da->setStoreAction(StoreAction(desc.depth_store)); da->setClearDepth(desc.clear_depth);
    auto* sa = rp->stencilAttachment(); sa->setTexture(target->texture.get());
    sa->setLoadAction(LoadAction(desc.stencil_load)); sa->setStoreAction(StoreAction(desc.stencil_store)); sa->setClearStencil(desc.clear_stencil);
  }
  encoder_ = frame_->command->renderCommandEncoder(rp.get());
  Require(encoder_, "Create render encoder"); encoder_->setLabel(Str(desc.label)); encoder_->pushDebugGroup(Str(desc.label));
  // Conservative initial contract across submissions and pass boundaries.
  // Narrow only after guest resource ownership is connected and validated.
  encoder_->barrierAfterQueueStages(MTL::StageAll, MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice);
  encoder_->setViewport(MTL::Viewport{0, 0, double(width_), double(height_), 0, 1});
  encoder_->setScissorRect(MTL::ScissorRect{0, 0, width_, height_});
}
void MetalBackend::Draw(const DrawDesc& desc) {
  Require(encoder_, "Draw outside render pass");
  auto pipeline = Cast<MetalPipeline>(desc.pipeline);
  Require(pipeline->desc.color == pass_format_ && pipeline->desc.depth_stencil == pass_depth_, "Pipeline attachment mismatch");
  auto bind_stage = [&](Stage stage, const MslShader& shader, MTL4::ArgumentTable* table) {
    for (const auto& required : shader.buffers) {
      // Xenos keeps descriptor-set/binding identities. Only this backend maps
      // them to the Metal slots selected by SPIRV-Cross.
      const BufferBinding* found = nullptr;
      for (const auto& binding : desc.buffers)
        if (binding.stage == stage && binding.descriptor_set == required.descriptor_set &&
            binding.binding == required.binding) {
          Require(!found, "Duplicate buffer binding"); found = &binding;
        }
      Require(found, "Required shader buffer binding missing");
      auto buffer = Cast<MetalBuffer>(found->buffer);
      Require(found->offset < buffer->size() && found->offset % 16 == 0, "Buffer binding offset invalid");
      table->setAddress(buffer->buffer->gpuAddress() + found->offset, required.metal_index); Retain(buffer);
    }
  };
  bind_stage(Stage::kVertex, pipeline->vertex, frame_->vertex.get());
  bind_stage(Stage::kFragment, pipeline->fragment, frame_->fragment.get());
  for (const auto& binding : desc.buffers) {
    if (binding.stage != Stage::kFragment || binding.descriptor_set != UINT32_MAX ||
        binding.binding != 30) continue;
    auto buffer = Cast<MetalBuffer>(binding.buffer);
    Require(binding.offset + 12 <= buffer->size(), "Fragment diagnostic buffer too small");
    frame_->fragment->setAddress(buffer->buffer->gpuAddress() + binding.offset, 30);
    Retain(buffer);
  }
  auto bind_images = [&](Stage stage, const MslShader& shader, MTL4::ArgumentTable* table) {
    for (const auto& required : shader.textures) {
      const TextureBinding* found = nullptr;
      for (const auto& binding : desc.textures)
        if (binding.stage == stage && binding.descriptor_set == required.descriptor_set && binding.binding == required.binding) {
          Require(!found, "Duplicate texture binding"); found = &binding;
        }
      Require(found, "Missing shader texture binding"); auto target = Cast<MetalTarget>(found->texture);
      Require(target->format != Format::kDepth32Stencil8, "Depth texture sampling is not connected yet");
      table->setTexture(target->texture->gpuResourceID(), required.metal_index); Retain(target);
    }
    for (const auto& required : shader.samplers) {
      const SamplerBinding* found = nullptr;
      for (const auto& binding : desc.samplers)
        if (binding.stage == stage && binding.descriptor_set == required.descriptor_set && binding.binding == required.binding) {
          Require(!found, "Duplicate sampler binding"); found = &binding;
        }
      Require(found, "Missing shader sampler binding");
      Require(found->filter == Filter::kLinear || found->filter == Filter::kNearest, "Invalid texture filter");
      MTL::SamplerState* sampler = samplers_[found->filter == Filter::kLinear][found->repeat].get();
      if (found->state) {
        const auto& s = *found->state;
        std::string key;
        for (auto v : {uint32_t(s.min_filter), uint32_t(s.mag_filter), s.mip_filter, s.min_lod, s.max_lod, s.anisotropy, uint32_t(s.u), uint32_t(s.v), uint32_t(s.w), s.border}) key += std::to_string(v) + ":";
        auto& cached = guest_samplers_[key];
        if (!cached) {
          auto sd = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
          sd->setMinFilter(s.min_filter == Filter::kLinear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
          sd->setMagFilter(s.mag_filter == Filter::kLinear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
          sd->setMipFilter(s.mip_filter == 2 ? MTL::SamplerMipFilterNotMipmapped : s.mip_filter == 1 ? MTL::SamplerMipFilterLinear : MTL::SamplerMipFilterNearest);
          const MTL::SamplerAddressMode modes[] = {MTL::SamplerAddressModeRepeat, MTL::SamplerAddressModeMirrorRepeat, MTL::SamplerAddressModeClampToEdge, MTL::SamplerAddressModeMirrorClampToEdge, MTL::SamplerAddressModeClampToBorderColor};
          Require(size_t(s.u) < std::size(modes) && size_t(s.v) < std::size(modes) && size_t(s.w) < std::size(modes), "Invalid sampler address mode");
          sd->setSAddressMode(modes[size_t(s.u)]); sd->setTAddressMode(modes[size_t(s.v)]); sd->setRAddressMode(modes[size_t(s.w)]);
          sd->setLodMinClamp(s.min_lod); sd->setLodMaxClamp(s.mip_filter == 2 ? s.min_lod + 0.25f : float(s.max_lod));
          sd->setMaxAnisotropy(s.anisotropy);
          sd->setBorderColor(s.border == 1 ? MTL::SamplerBorderColorOpaqueWhite : MTL::SamplerBorderColorTransparentBlack);
          sd->setSupportArgumentBuffers(true); sd->setLabel(Str("Xenos sampler " + key));
          cached = NS::TransferPtr(device_->newSamplerState(sd.get())); Require(bool(cached), "Guest sampler creation failed");
        }
        sampler = cached.get();
      }
      table->setSamplerState(sampler->gpuResourceID(), required.metal_index);
    }
  };
  bind_images(Stage::kVertex, pipeline->vertex, frame_->vertex.get());
  bind_images(Stage::kFragment, pipeline->fragment, frame_->fragment.get());
  const auto vp = desc.viewport.width && desc.viewport.height ? desc.viewport : Rectangle{0, 0, width_, height_};
  const auto sc = desc.scissor.width && desc.scissor.height ? desc.scissor : Rectangle{0, 0, width_, height_};
  Require(uint64_t(vp.x) + vp.width <= width_ && uint64_t(vp.y) + vp.height <= height_ &&
      uint64_t(sc.x) + sc.width <= width_ && uint64_t(sc.y) + sc.height <= height_, "Draw rectangle exceeds target");
  encoder_->setViewport(MTL::Viewport{double(vp.x), double(vp.y), double(vp.width), double(vp.height), desc.depth_min, desc.depth_max});
  encoder_->setScissorRect(MTL::ScissorRect{sc.x, sc.y, sc.width, sc.height});
  encoder_->setArgumentTable(frame_->vertex.get(), MTL::RenderStageVertex);
  encoder_->setArgumentTable(frame_->fragment.get(), MTL::RenderStageFragment);
  encoder_->setRenderPipelineState(pipeline->state.get()); encoder_->setDepthStencilState(pipeline->depth.get());
  encoder_->setStencilReferenceValue(desc.stencil_reference);
  Require(!(desc.cull_front && desc.cull_back), "Both faces culled: discard this draw in the frontend");
  encoder_->setCullMode(desc.cull_front ? MTL::CullModeFront : desc.cull_back ? MTL::CullModeBack : MTL::CullModeNone);
  encoder_->setFrontFacingWinding(desc.front_clockwise ? MTL::WindingClockwise : MTL::WindingCounterClockwise);
  encoder_->setBlendColor(desc.blend_constant[0], desc.blend_constant[1], desc.blend_constant[2], desc.blend_constant[3]);
  Retain(pipeline);
  const MTL::PrimitiveType types[] = {MTL::PrimitiveTypePoint, MTL::PrimitiveTypeLine,
      MTL::PrimitiveTypeLineStrip, MTL::PrimitiveTypeTriangle, MTL::PrimitiveTypeTriangleStrip};
  auto primitive = static_cast<size_t>(desc.primitive);
  Require(primitive < std::size(types), "Invalid primitive type");
  if (desc.indices) {
    auto index = Cast<MetalBuffer>(desc.indices);
    const uint64_t index_size = desc.indices32 ? 4 : 2, offset = uint64_t(desc.first) * index_size;
    Require(offset <= index->size() && uint64_t(desc.count) * index_size <= index->size() - offset, "Index range out of bounds");
    encoder_->drawIndexedPrimitives(types[primitive], desc.count, desc.indices32 ? MTL::IndexTypeUInt32 : MTL::IndexTypeUInt16,
        index->buffer->gpuAddress() + offset, index->size() - offset, 1, desc.base_vertex, 0);
    Retain(index);
  } else encoder_->drawPrimitives(types[primitive], desc.first, desc.count);
  ++draws_;
}
void MetalBackend::EndPass() {
  Require(encoder_, "No render pass to end");
  encoder_->barrierAfterStages(MTL::StageVertex | MTL::StageFragment, MTL::StageAll, MTL4::VisibilityOptionDevice);
  encoder_->popDebugGroup(); encoder_->endEncoding(); encoder_ = nullptr;
}
void MetalBackend::Submit() {
  Require(frame_ && !encoder_, "Submit requires a closed render pass and open frame");
  frame_->command->endCommandBuffer();
  {
    const auto begin = std::chrono::steady_clock::now();
    residency_->Commit();
    residency_commit_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
  }
  if (drawable_) queue_->wait(drawable_);
  const MTL4::CommandBuffer* commands[] = {frame_->command.get()};
  {
    const auto begin = std::chrono::steady_clock::now();
    queue_->commit(commands, 1);
    queue_submit_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count()), std::memory_order_relaxed);
  }
  if (drawable_) { queue_->signalDrawable(drawable_); drawable_->present(); }
  frame_->completion = ++submissions_; queue_->signalEvent(event_.get(), submissions_);
  frame_ = nullptr; drawable_ = nullptr; frame_pool_.reset();
}
void MetalBackend::WaitIdle() {
  Require(!frame_, "WaitIdle cannot discard an open frame"); Wait(submissions_);
}
std::vector<uint8_t> MetalBackend::Readback(const std::shared_ptr<Target>& input) {
  Require(!frame_, "Readback requires submitted rendering");
  auto target = Cast<MetalTarget>(input);
  Require(target->format == Format::kRGBA8 || target->format == Format::kBGRA8, "Readback supports RGBA8/BGRA8 only");
  WaitIdle();
  const size_t pitch = (size_t(target->width()) * 4 + 255) & ~size_t(255);
  std::vector<std::byte> empty(pitch * target->height());
  auto readback = Cast<MetalBuffer>(CreateBuffer(empty, "MGR target readback"));
  Require(BeginCommands(false), "Readback could not acquire command slot");
  Retain(target); Retain(readback);
  auto* copy = frame_->command->computeCommandEncoder();
  Require(copy, "Create readback encoder"); copy->setLabel(Str("MGR diagnostic readback"));
  copy->barrierAfterQueueStages(MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice);
  copy->copyFromTexture(target->texture.get(), 0, 0, MTL::Origin(0, 0, 0),
      MTL::Size(target->width(), target->height(), 1), readback->buffer.get(), 0, pitch, pitch * target->height());
  copy->endEncoding(); Submit(); WaitIdle();
  std::vector<uint8_t> bytes(size_t(target->width()) * target->height() * 4);
  auto* source = static_cast<const uint8_t*>(readback->buffer->contents());
  for (uint32_t y = 0; y < target->height(); ++y)
    std::memcpy(bytes.data() + size_t(y) * target->width() * 4, source + size_t(y) * pitch, size_t(target->width()) * 4);
  if (target->format == Format::kBGRA8)
    for (size_t i = 0; i < bytes.size(); i += 4) std::swap(bytes[i], bytes[i + 2]);
  return bytes;
}
void MetalBackend::EnsureResolvePipeline() {
  if (resolve_pipeline_) return;
  auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
  options->setMathMode(MTL::MathModeSafe);
  options->setLanguageVersion(MTL::LanguageVersion3_0);
  NS::Error* error = nullptr;
  auto library = NS::TransferPtr(device_->newLibrary(Str(kResolveEdramMsl), options.get(), &error));
  Require(bool(library), "EDRAM resolve MSL compilation: " + Error(error));
  auto function = NS::TransferPtr(library->newFunction(Str("resolve_edram")));
  Require(bool(function), "EDRAM resolve kernel not found");
  resolve_pipeline_ = NS::TransferPtr(device_->newComputePipelineState(function.get(), &error));
  Require(bool(resolve_pipeline_), "EDRAM resolve pipeline: " + Error(error));
}
void MetalBackend::ResolveEdram(const std::shared_ptr<Buffer>& edram,
                                const std::shared_ptr<Buffer>& guest_memory,
                                const EdramResolveParams& params) {
  Require(!frame_, "ResolveEdram requires a submitted frame");
  Require(params.width && params.height, "Empty EDRAM resolve");
  EnsureResolvePipeline();
  auto edram_buffer = Cast<MetalBuffer>(edram);
  auto guest_buffer = Cast<MetalBuffer>(guest_memory);
  if (!resolve_params_) resolve_params_ = CreateBufferSized(sizeof(params), "MGR resolve params");
  UpdateBuffer(resolve_params_, 0, std::as_bytes(std::span(&params, 1)));
  const auto dispatch_begin = std::chrono::steady_clock::now();
  Require(BeginCommands(false), "Begin EDRAM resolve dispatch");
  Retain(edram); Retain(guest_memory); Retain(resolve_params_);
  auto* encoder = frame_->command->computeCommandEncoder();
  Require(encoder, "Create EDRAM resolve encoder");
  encoder->setLabel(Str("MGR EDRAM resolve"));
  frame_->compute->setAddress(edram_buffer->buffer->gpuAddress(), 0);
  frame_->compute->setAddress(guest_buffer->buffer->gpuAddress(), 1);
  frame_->compute->setAddress(Cast<MetalBuffer>(resolve_params_)->buffer->gpuAddress(), 2);
  encoder->setComputePipelineState(resolve_pipeline_.get());
  encoder->setArgumentTable(frame_->compute.get());
  encoder->barrierAfterQueueStages(MTL::StageAll, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  encoder->dispatchThreads(MTL::Size(params.width, params.height, 1), MTL::Size(8, 8, 1));
  encoder->barrierAfterStages(MTL::StageDispatch, MTL::StageAll, MTL4::VisibilityOptionDevice);
  encoder->endEncoding();
  resolve_dispatch_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - dispatch_begin).count()), std::memory_order_relaxed);
  const auto commit_begin = std::chrono::steady_clock::now();
  Submit();
  resolve_commit_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - commit_begin).count()), std::memory_order_relaxed);
  const auto wait_begin = std::chrono::steady_clock::now();
  WaitIdle();
  resolve_wait_us_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - wait_begin).count()), std::memory_order_relaxed);
  resolve_dispatches_.fetch_add(1, std::memory_order_relaxed);
}
void MetalBackend::FlushPipelineCache() {
  if (archive_path_.empty()) return;
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  std::lock_guard lock(compiler_mutex_);
  if (!cache_dirty_) return;
  auto temporary = archive_path_.parent_path() / ("pipelines." + std::to_string(getpid()) + ".tmp.metallib");
  NS::Error* error = nullptr;
  if (!serializer_->serializeAsArchiveAndFlushToURL(FileUrl(temporary), &error)) {
    std::fprintf(stderr, "[metal4] pipeline cache write failed: %s\n", Error(error).c_str()); return;
  }
  const auto digest = FileDigest(temporary);
  auto checksum_temporary = temporary; checksum_temporary += ".sha256";
  std::ofstream checksum(checksum_temporary, std::ios::binary);
  checksum << digest << '\n'; checksum.close();
  if (digest.empty() || !checksum) {
    std::fprintf(stderr, "[metal4] pipeline cache write failed: checksum could not be saved\n"); return;
  }
  // A failed write never replaces the previous working archive.
  std::error_code ec; std::filesystem::rename(temporary, archive_path_, ec);
  if (!ec) std::filesystem::rename(checksum_temporary, archive_path_.string() + ".sha256", ec);
  if (ec) std::fprintf(stderr, "[metal4] pipeline cache write failed: %s\n", ec.message().c_str());
  else cache_dirty_ = false;
}
}  // namespace
std::unique_ptr<HostBackend> CreateMetal4Backend(const BackendConfig& config) {
  return std::make_unique<MetalBackend>(config);
}
void ValidateMslWithMetal(const MslShader& shader) {
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
  Require(device && device->supportsFamily(MTL::GPUFamilyMetal4), "Metal 4 device unavailable");
  auto descriptor = NS::TransferPtr(MTL4::CompilerDescriptor::alloc()->init());
  NS::Error* error = nullptr;
  auto compiler = NS::TransferPtr(device->newCompiler(descriptor.get(), &error));
  Require(bool(compiler), "Create shader probe compiler: " + Error(error));
  auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
  options->setMathMode(MTL::MathModeSafe); options->setLanguageVersion(MTL::LanguageVersion3_2);
  auto library_desc = NS::TransferPtr(MTL4::LibraryDescriptor::alloc()->init());
  library_desc->setSource(Str(shader.source)); library_desc->setOptions(options.get());
  auto library = NS::TransferPtr(compiler->newLibrary(library_desc.get(), &error));
  Require(bool(library), "Generated MSL library compilation: " + Error(error));
}
}  // namespace mgr::gpu
