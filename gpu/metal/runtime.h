#pragma once
#include "host_backend.h"
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#include <map>
#include <mutex>

namespace mgr::gpu::metal {
struct DeviceContext {
  std::recursive_mutex mutex;
  std::unique_ptr<HostBackend> backend;
};
class MetalProvider final : public rex::ui::GraphicsProvider {
 public:
  explicit MetalProvider(std::filesystem::path cache = {});
  std::unique_ptr<rex::ui::Presenter> CreatePresenter(rex::ui::Presenter::HostGpuLossCallback) override;
  std::unique_ptr<rex::ui::ImmediateDrawer> CreateImmediateDrawer() override;
  std::shared_ptr<DeviceContext> context() const { return context_; }
 private:
  std::shared_ptr<DeviceContext> context_;
};
class MetalGraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  ~MetalGraphicsSystem() override { Shutdown(); }
  std::string name() const override { return "Metal 4 (experimental Xenos backend)"; }
  MetalProvider* metal_provider() const { return static_cast<MetalProvider*>(provider_.get()); }
 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;
};
// The mailbox payload is a host texture. Its creation/submission remains the
// command processor's responsibility; presentation retains it through use.
class MetalRefreshContext final : public rex::ui::Presenter::GuestOutputRefreshContext {
 public:
  explicit MetalRefreshContext(bool& is_8bpc) : GuestOutputRefreshContext(is_8bpc) {}
  std::shared_ptr<Target> image;
};
class MetalPresenter final : public rex::ui::Presenter {
 public:
  MetalPresenter(std::shared_ptr<DeviceContext>, HostGpuLossCallback);
  ~MetalPresenter() override;
  bool Initialize();
  rex::ui::Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(rex::ui::RawImage&) override;
 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(rex::ui::Surface&, uint32_t, uint32_t, bool, bool&) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t, uint32_t, uint32_t,
      std::function<bool(GuestOutputRefreshContext&)>, bool&) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;
 private:
  std::shared_ptr<DeviceContext> context_;
  std::array<std::shared_ptr<Target>, kGuestOutputMailboxSize> images_;
  std::shared_ptr<Pipeline> pipeline_;
  uint32_t width_ = 0, height_ = 0;
};
struct FrontendStats { uint64_t shader_loads = 0, swaps = 0, draws = 0, resolves = 0, unsupported_draws = 0, unsupported_resolves = 0; };
class MetalCommandProcessor : public rex::graphics::CommandProcessor {
 public:
  MetalCommandProcessor(MetalGraphicsSystem*, rex::system::KernelState*);
  ~MetalCommandProcessor() override;
  void IssueSwap(uint32_t, uint32_t, uint32_t) override;
  void InitializeShaderStorage(const std::filesystem::path&, uint32_t, bool) override;
  FrontendStats frontend_stats() const { return stats_; }
 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override;
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override;
  bool IssueCopy() override;
 private:
  [[noreturn]] void Unsupported(const char* operation);
  std::shared_ptr<DeviceContext> context_;
  std::map<std::pair<rex::graphics::xenos::ShaderType, uint64_t>, std::unique_ptr<rex::graphics::Shader>> shaders_;
  struct GuestRenderer;
  std::unique_ptr<GuestRenderer> guest_;
  FrontendStats stats_;
};
}  // namespace mgr::gpu::metal
