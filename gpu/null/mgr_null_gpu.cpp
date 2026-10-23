// rexgpu-mgrnull: the guest-visible half of the Xenos GPU without rendering.
//
// The native renderer (src/native) replaces the game's D3D draw path, but the
// game still paces itself on GPU services: the vblank interrupt (callback
// 0x826B3FE0), fences written by EVENT_WRITE_SHD/MEM_WRITE, the ring-buffer
// read pointer write-back and the interrupt status MMIO read. This plugin keeps
// the SDK GraphicsSystem (vblank thread, MMIO) and the SDK CommandProcessor (PM4
// parsing, fences, interrupts, indirect buffers) exactly as the Xenos plugin
// runs them, and turns every draw, resolve, shader load and swap into a no-op.

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include <memory>
#include <string>

namespace mgr::gpu {

using rex::X_STATUS;  // X_STATUS_SUCCESS expands to an unqualified cast

class NullCommandProcessor final : public rex::graphics::CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType, uint32_t, const uint32_t*,
                                    uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*,
                 bool) override {
    return true;
  }
  bool IssueCopy() override { return true; }
};

class NullGraphicsSystem final : public rex::graphics::GraphicsSystem {
 public:
  std::string name() const override { return "mgr-null"; }

  // The native renderer owns the window surface; there is no SDK presenter.
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    return X_STATUS_SUCCESS;
  }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NullCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace mgr::gpu

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    REXLOG_ERROR("rexgpu-mgrnull: host requested ABI {}, plugin is ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-mgrnull: invalid GpuCreateInfo");
    return nullptr;
  }
  return new mgr::gpu::NullGraphicsSystem();
}
