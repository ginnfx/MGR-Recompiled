#include "runtime.h"
#include "runtime_shaders.h"
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/surface_mac.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace mgr::gpu::metal {
namespace {
template<size_t N> ShaderCode Shader(const uint32_t (&words)[N], Stage stage) {
  return {std::vector<uint32_t>(words, words + N), "main", stage};
}
PipelineDesc PresentPipeline() {
  PipelineDesc d; d.vertex = Shader(shaders::present_vertex, Stage::kVertex);
  d.fragment = Shader(shaders::ui_fragment, Stage::kFragment);
  d.color = Format::kBGRA8; d.label = "MGR Metal guest output"; return d;
}
class MetalUIContext final : public rex::ui::UIDrawContext {
 public:
  MetalUIContext(rex::ui::Presenter& p, uint32_t w, uint32_t h) : UIDrawContext(p, w, h) {}
};
class MetalImmediateTexture final : public rex::ui::ImmediateTexture {
 public:
  MetalImmediateTexture(uint32_t w, uint32_t h) : ImmediateTexture(w, h) {}
  std::shared_ptr<Target> image;
  Filter filter = Filter::kLinear;
  bool repeat = false;
};
class MetalImmediateDrawer final : public rex::ui::ImmediateDrawer {
 public:
  explicit MetalImmediateDrawer(std::shared_ptr<DeviceContext> context) : context_(std::move(context)) {
    std::lock_guard lock(context_->mutex);
    auto d = PresentPipeline(); d.vertex = Shader(shaders::ui_vertex, Stage::kVertex);
    d.blend = true; d.source_rgb = BlendFactor::kSourceAlpha; d.destination_rgb = BlendFactor::kOneMinusSourceAlpha;
    d.source_alpha = BlendFactor::kOne; d.destination_alpha = BlendFactor::kOneMinusSourceAlpha;
    d.label = "MGR Metal host UI"; pipeline_ = context_->backend->PreparePipeline(d).get();
    const uint32_t white = 0xffffffff;
    white_ = context_->backend->CreateTextureRGBA8(1, 1, std::as_bytes(std::span(&white, 1)), "MGR white UI texture");
  }
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t w, uint32_t h,
      rex::ui::ImmediateTextureFilter filter, bool repeat, const uint8_t* data) override {
    if (!data || !w || !h) return nullptr;
    auto result = std::make_unique<MetalImmediateTexture>(w, h);
    std::lock_guard lock(context_->mutex);
    result->image = context_->backend->CreateTextureRGBA8(w, h,
        std::as_bytes(std::span(data, size_t(w) * h * 4)), "MGR host UI texture");
    result->filter = filter == rex::ui::ImmediateTextureFilter::kNearest ? Filter::kNearest : Filter::kLinear;
    result->repeat = repeat; return result;
  }
  void Begin(rex::ui::UIDrawContext& context, float w, float h) override {
    if (!dynamic_cast<MetalUIContext*>(&context)) throw std::runtime_error("Metal UI used with a non-Metal presenter");
    ImmediateDrawer::Begin(context, w, h);
    const float projection[] = {2 / coordinate_space_width(), -2 / coordinate_space_height(), -1, 1};
    projection_ = context_->backend->CreateBuffer(std::as_bytes(std::span(projection)), "MGR host UI projection");
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    if (batch.vertex_count <= 0 || !batch.vertices) throw std::runtime_error("Empty Metal UI batch");
    vertices_ = context_->backend->CreateBuffer(
        std::as_bytes(std::span(batch.vertices, size_t(batch.vertex_count))), "MGR host UI vertices");
    indices_.reset();
    if (batch.index_count > 0) {
      if (!batch.indices) throw std::runtime_error("Missing Metal UI indices");
      indices_ = context_->backend->CreateBuffer(
          std::as_bytes(std::span(batch.indices, size_t(batch.index_count))), "MGR host UI indices");
    }
    vertex_count_ = batch.vertex_count;
  }
  void Draw(const rex::ui::ImmediateDraw& immediate) override {
    if (immediate.count <= 0) return;
    if (!vertices_ || immediate.index_offset < 0 || immediate.base_vertex < 0 ||
        (!indices_ && uint64_t(immediate.base_vertex) + immediate.count > vertex_count_))
      throw std::runtime_error("Invalid Metal UI draw range");
    uint32_t x, y, w, h;
    if (!ScissorToRenderTarget(immediate, x, y, w, h)) return;
    DrawDesc d; d.pipeline = pipeline_; d.count = immediate.count; d.scissor = {x, y, w, h};
    d.primitive = immediate.primitive_type == rex::ui::ImmediatePrimitiveType::kLines ? Primitive::kLines : Primitive::kTriangles;
    d.indices = indices_; d.first = indices_ ? immediate.index_offset : immediate.base_vertex;
    d.base_vertex = indices_ ? immediate.base_vertex : 0;
    d.buffers = {{Stage::kVertex, 0, 0, vertices_, 0}, {Stage::kVertex, 0, 1, projection_, 0}};
    std::shared_ptr<Target> texture = white_; Filter filter = Filter::kNearest; bool repeat = false;
    if (immediate.texture) {
      auto* native = dynamic_cast<MetalImmediateTexture*>(immediate.texture);
      if (!native) throw std::runtime_error("Non-Metal UI texture");
      texture = native->image; filter = native->filter; repeat = native->repeat;
    }
    d.textures = {{Stage::kFragment, 0, 2, texture}};
    d.samplers = {{Stage::kFragment, 0, 3, filter, repeat}};
    context_->backend->Draw(d);
  }
  void EndDrawBatch() override { vertices_.reset(); indices_.reset(); }
  void End() override { projection_.reset(); ImmediateDrawer::End(); }
 private:
  std::shared_ptr<DeviceContext> context_;
  std::shared_ptr<Pipeline> pipeline_;
  std::shared_ptr<Target> white_;
  std::shared_ptr<Buffer> vertices_, indices_, projection_;
  uint64_t vertex_count_ = 0;
};
}  // namespace

MetalProvider::MetalProvider(std::filesystem::path cache) : context_(std::make_shared<DeviceContext>()) {
  BackendConfig config; config.cache_directory = std::move(cache);
  context_->backend = CreateMetal4Backend(config);
  REXLOG_INFO("MGR Metal 4 provider: {}", context_->backend->device_name());
}
std::unique_ptr<rex::ui::Presenter> MetalProvider::CreatePresenter(rex::ui::Presenter::HostGpuLossCallback callback) {
  auto result = std::make_unique<MetalPresenter>(context_, std::move(callback));
  if (!result->Initialize()) return nullptr;
  return result;
}
std::unique_ptr<rex::ui::ImmediateDrawer> MetalProvider::CreateImmediateDrawer() {
  return std::make_unique<MetalImmediateDrawer>(context_);
}
void MetalGraphicsSystem::CreateProvider(bool) {
  try {
    const std::filesystem::path cache = REXCVAR_GET(cache_root);
    provider_ = std::make_unique<MetalProvider>(cache.empty() ? cache : cache / "metal4");
  }
  catch (const std::exception& e) { REXLOG_ERROR("Metal provider initialization failed: {}", e.what()); provider_.reset(); }
}
std::unique_ptr<rex::graphics::CommandProcessor> MetalGraphicsSystem::CreateCommandProcessor() {
  if (!metal_provider()) throw std::runtime_error("Metal graphics provider unavailable");
  return std::make_unique<MetalCommandProcessor>(this, kernel_state_);
}
MetalPresenter::MetalPresenter(std::shared_ptr<DeviceContext> context, HostGpuLossCallback callback)
    : Presenter(std::move(callback)), context_(std::move(context)) {}
bool MetalPresenter::Initialize() {
  std::lock_guard lock(context_->mutex);
  pipeline_ = context_->backend->PreparePipeline(PresentPipeline()).get();
  return InitializeCommonSurfaceIndependent();
}
MetalPresenter::~MetalPresenter() {
  SetWindowSurfaceFromUIThread(nullptr, nullptr);
  std::lock_guard lock(context_->mutex); context_->backend->WaitIdle();
}
rex::ui::Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
  return rex::ui::Surface::kTypeFlag_CAMetalLayer;
}
MetalPresenter::SurfacePaintConnectResult MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    rex::ui::Surface& surface, uint32_t w, uint32_t h, bool, bool& implicit_vsync) {
  if (surface.GetType() != rex::ui::Surface::kTypeIndex_CAMetalLayer) return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  try {
    std::lock_guard lock(context_->mutex);
    context_->backend->AttachSurface(static_cast<rex::ui::CAMetalLayerSurface&>(surface).layer(), w, h);
    width_ = w; height_ = h;
    // Keep SDL/window APIs on the UI thread. Guest output only publishes to
    // the existing mailbox and requests a UI paint.
    implicit_vsync = true; return SurfacePaintConnectResult::kSuccess;
  } catch (const std::exception& e) {
    REXLOG_ERROR("Metal surface connection failed: {}", e.what());
    DisconnectPaintingFromSurfaceFromUIThreadImpl(); return SurfacePaintConnectResult::kFailure;
  }
}
void MetalPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  std::lock_guard lock(context_->mutex); context_->backend->DetachSurface(); width_ = height_ = 0;
}
bool MetalPresenter::RefreshGuestOutputImpl(uint32_t index, uint32_t w, uint32_t h,
    std::function<bool(GuestOutputRefreshContext&)> refresher, bool& is_8bpc) {
  MetalRefreshContext context(is_8bpc);
  if (!refresher(context) || !context.image || context.image->width() != w || context.image->height() != h) return false;
  images_[index] = std::move(context.image); return true;
}
bool MetalPresenter::CaptureGuestOutput(rex::ui::RawImage& output) {
  std::shared_ptr<Target> image;
  { uint32_t index; auto consumer = ConsumeGuestOutput(index, nullptr, nullptr);
    if (index == UINT32_MAX) return false; image = images_[index]; }
  if (!image) return false;
  std::lock_guard lock(context_->mutex);
  auto bytes = context_->backend->Readback(image);
  output.width = image->width(); output.height = image->height(); output.stride = size_t(output.width) * 4;
  output.data = std::move(bytes); return true;
}
MetalPresenter::PaintResult MetalPresenter::PaintAndPresentImpl(bool ui) {
  std::shared_ptr<Target> image;
  GuestOutputProperties properties; GuestOutputPaintConfig config;
  { uint32_t index; auto consumer = ConsumeGuestOutput(index, &properties, &config);
    if (index != UINT32_MAX) image = images_[index]; }
  std::lock_guard lock(context_->mutex);
  try {
    if (!context_->backend->BeginFrame()) return PaintResult::kNotPresented;
    PassDesc pass; pass.label = "MGR Metal guest and host UI";
    context_->backend->BeginPass(pass);
    if (image && properties.IsActive()) {
      auto flow = GetGuestOutputPaintFlow(properties, width_, height_, 16384, 16384, config);
      if (flow.effect_count != 1 || flow.effects[0] != GuestOutputPaintEffect::kBilinear)
        throw std::runtime_error("Unsupported native presentation effect");
      if (flow.effect_count == 1) {
        auto [w, h] = flow.effect_output_sizes[0];
        DrawDesc draw; draw.pipeline = pipeline_; draw.count = 3;
        draw.viewport = {uint32_t(flow.output_x), uint32_t(flow.output_y), w, h};
        draw.textures = {{Stage::kFragment, 0, 2, image}};
        draw.samplers = {{Stage::kFragment, 0, 3, Filter::kLinear, false}};
        context_->backend->Draw(draw);
      }
    }
    if (ui) { MetalUIContext context(*this, width_, height_); ExecuteUIDrawersFromUIThread(context); }
    context_->backend->EndPass(); context_->backend->Submit(); return PaintResult::kPresented;
  } catch (const std::exception& e) {
    REXLOG_ERROR("Metal presentation failed: {}", e.what()); return PaintResult::kGpuLostResponsible;
  }
}
}  // namespace mgr::gpu::metal
