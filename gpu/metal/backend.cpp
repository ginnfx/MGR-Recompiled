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
    number(shader->stage); number(shader->entry.size()); data += shader->entry;
    number(shader->spirv.size());
    if (!shader->spirv.empty())
      data.append(reinterpret_cast<const char*>(shader->spirv.data()), shader->spirv.size() * 4);
  }
  number(d.color); number(d.depth_stencil); number(d.depth_write); number(d.depth_compare);
  number(d.stencil); number(d.stencil_compare); number(d.stencil_pass);
  number(d.stencil_read_mask); number(d.stencil_write_mask);
  number(d.blend); number(d.source_rgb); number(d.destination_rgb);
  number(d.source_alpha); number(d.destination_alpha); number(d.color_write_mask);
  return Digest(data);
}
MTL::PixelFormat PixelFormat(Format f) {
  switch (f) {
    case Format::kRGBA8: return MTL::PixelFormatRGBA8Unorm;
    case Format::kBGRA8: return MTL::PixelFormatBGRA8Unorm;
    case Format::kRGBA16Float: return MTL::PixelFormatRGBA16Float;
    case Format::kDepth32Stencil8: return MTL::PixelFormatDepth32Float_Stencil8;
  }
  throw std::runtime_error("Unknown host pixel format");
}
MTL::BlendFactor Blend(BlendFactor f) {
  switch (f) {
    case BlendFactor::kZero: return MTL::BlendFactorZero;
    case BlendFactor::kOne: return MTL::BlendFactorOne;
    case BlendFactor::kSourceAlpha: return MTL::BlendFactorSourceAlpha;
    case BlendFactor::kOneMinusSourceAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
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
  void Add(MTL::Allocation* allocation) {
    std::lock_guard lock(mutex); set->addAllocation(allocation); dirty = true;
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
  NS::SharedPtr<MTL4::ArgumentTable> vertex, fragment;
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
  std::shared_ptr<Target> CreateTarget(const TargetDesc&) override;
  std::shared_future<std::shared_ptr<Pipeline>> PreparePipeline(PipelineDesc) override;
  bool BeginFrame(bool present) override;
  void BeginPass(const PassDesc&) override;
  void Draw(const DrawDesc&) override;
  void EndPass() override;
  void Submit() override;
  void WaitIdle() override;
  std::vector<uint8_t> Readback(const std::shared_ptr<Target>&) override;
  void FlushPipelineCache() override;
  BackendStats stats() const override { return {submissions_, draws_, pipelines_created_.load(), archive_loaded_}; }

 private:
  template<class T, class U> std::shared_ptr<T> Cast(const std::shared_ptr<U>& r) {
    auto p = std::dynamic_pointer_cast<T>(r);
    Require(p && p->owner == this, "Resource belongs to a different backend");
    return p;
  }
  void Wait(uint64_t value);
  bool BeginCommands(bool acquire_drawable);
  std::shared_ptr<Pipeline> Compile(const PipelineDesc&);
  void Worker();
  void Retain(const std::shared_ptr<Resource>& r) { frame_->retained.push_back(r); }
  BackendConfig config_;
  NS::SharedPtr<MTL::Device> device_;
  NS::SharedPtr<MTL4::CommandQueue> queue_;
  NS::SharedPtr<MTL::SharedEvent> event_;
  std::shared_ptr<Residency> residency_;
  SdlMetalView view_;
  CA::MetalLayer* layer_ = nullptr;  // owned by the SDL view, destroyed after GPU idle
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
    f.vertex = NS::TransferPtr(device_->newArgumentTable(ad.get(), &error));
    f.fragment = NS::TransferPtr(device_->newArgumentTable(ad.get(), &error));
    Require(f.vertex && f.fragment, "Create argument tables: " + Error(error));
  }
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

std::shared_ptr<Target> MetalBackend::CreateTarget(const TargetDesc& desc) {
  Require(desc.width && desc.height, "Render target dimensions must be nonzero");
  auto pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
  auto td = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
  td->setTextureType(MTL::TextureType2D); td->setWidth(desc.width); td->setHeight(desc.height);
  td->setPixelFormat(PixelFormat(desc.format)); td->setStorageMode(MTL::StorageModePrivate);
  td->setUsage(MTL::TextureUsageRenderTarget);
  auto handle = NS::TransferPtr(device_->newTexture(td.get()));
  Require(bool(handle), "Render target allocation failed: " + desc.label); handle->setLabel(Str(desc.label));
  auto result = std::make_shared<MetalTarget>();
  result->owner = this; result->residency = residency_; result->texture = std::move(handle); result->format = desc.format;
  residency_->Add(result->texture.get()); return result;
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
  Require(!result->vertex.needs_extended_bindings && !result->fragment.needs_extended_bindings,
          "Draw pipeline requires texture, sampler or push-constant bindings not connected yet");
  auto library = [&](const MslShader& msl) {
    auto options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
    options->setMathMode(MTL::MathModeSafe);
    options->setLanguageVersion(MTL::LanguageVersion3_0);
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
  pd->setRasterSampleCount(1);
  auto* color = pd->colorAttachments()->object(0);
  color->setPixelFormat(PixelFormat(desc.color));
  color->setBlendingState(desc.blend ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);
  color->setSourceRGBBlendFactor(Blend(desc.source_rgb)); color->setDestinationRGBBlendFactor(Blend(desc.destination_rgb));
  color->setSourceAlphaBlendFactor(Blend(desc.source_alpha)); color->setDestinationAlphaBlendFactor(Blend(desc.destination_alpha));
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
  ++pipelines_created_; cache_dirty_ = true; return result;
}

void MetalBackend::Wait(uint64_t value) {
  if (value && !event_->waitUntilSignaledValue(value, 30000))
    throw std::runtime_error("GPU submission failed to complete within 30 seconds");
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
    int w = 0, h = 0;
    Require(SDL_GetWindowSizeInPixels(config_.window, &w, &h), SDL_GetError());
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(config_.window) & SDL_WINDOW_MINIMIZED)) {
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
  MTL::Texture* color;
  if (desc.color) {
    auto target = Cast<MetalTarget>(desc.color);
    Require(target->format != Format::kDepth32Stencil8, "Depth texture used as color");
    color = target->texture.get(); pass_format_ = target->format; Retain(target);
  } else {
    Require(drawable_, "No drawable for window pass");
    color = drawable_->texture(); pass_format_ = Format::kBGRA8;
  }
  width_ = static_cast<uint32_t>(color->width()); height_ = static_cast<uint32_t>(color->height());
  auto rp = NS::TransferPtr(MTL4::RenderPassDescriptor::alloc()->init());
  auto* ca = rp->colorAttachments()->object(0);
  ca->setTexture(color); ca->setLoadAction(LoadAction(desc.color_load)); ca->setStoreAction(StoreAction(desc.color_store));
  const auto& c = desc.clear_color; ca->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
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
  encoder_->setArgumentTable(frame_->vertex.get(), MTL::RenderStageVertex);
  encoder_->setArgumentTable(frame_->fragment.get(), MTL::RenderStageFragment);
  encoder_->setRenderPipelineState(pipeline->state.get()); encoder_->setDepthStencilState(pipeline->depth.get());
  encoder_->setStencilReferenceValue(desc.stencil_reference);
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
        index->buffer->gpuAddress() + offset, index->size() - offset);
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
  frame_->command->endCommandBuffer(); residency_->Commit();
  if (drawable_) queue_->wait(drawable_);
  const MTL4::CommandBuffer* commands[] = {frame_->command.get()};
  queue_->commit(commands, 1);
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
  options->setMathMode(MTL::MathModeSafe); options->setLanguageVersion(MTL::LanguageVersion3_0);
  auto library_desc = NS::TransferPtr(MTL4::LibraryDescriptor::alloc()->init());
  library_desc->setSource(Str(shader.source)); library_desc->setOptions(options.get());
  auto library = NS::TransferPtr(compiler->newLibrary(library_desc.get(), &error));
  Require(bool(library), "Generated MSL library compilation: " + Error(error));
}
}  // namespace mgr::gpu
