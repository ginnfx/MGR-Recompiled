#include "mgr_native.h"

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/presenter.h>

#include "core/logging.h"
#include "gpu/capture.h"
#include "gpu/device.h"
#include "platform/native_window.h"

REXCVAR_DEFINE_STRING(mgr_renderer, "xenos", "MGR/Graphics",
                      "Renderer: xenos (GPU emulation) or native (D3D-hook renderer, experimental). "
                      "Applies on the next launch.");

namespace mgrn {

namespace {
std::atomic<bool> g_active{false};
constexpr const char* kNullGpuPlugin = "mgrnull";
// Long enough for a title-screen frame at 30 fps plus a shader-compile hitch.
constexpr uint32_t kCaptureTimeoutMs = 2000;
}  // namespace

bool NativeRendererActive() { return g_active.load(std::memory_order_relaxed); }

void MgrNativeConfigure(rex::RuntimeConfig& config) {
  const bool native = REXCVAR_GET(mgr_renderer) == "native";
  g_active.store(native, std::memory_order_relaxed);
  if (!native) return;
  config.gpu_plugin = kNullGpuPlugin;
  MGRN_INFO("Native renderer selected (GPU services: rexgpu-{})", kNullGpuPlugin);
}

bool MgrNativeStart(rex::ui::Window* window) {
  if (!NativeRendererActive()) return true;
  if (!platform::PrepareVulkanLoader()) return false;
  if (!gpu::Video::CreateHostDevice(window)) {
    MGRN_ERROR("Native renderer: device creation failed");
    return false;
  }
  MGRN_INFO("Native renderer ready: {} on {}", gpu::Video::GetBackendInfo(),
            gpu::Video::GetDeviceName());
  return true;
}

bool MgrNativeCapture(rex::ui::RawImage& image) {
  gpu::CapturedFrame frame;
  if (!gpu::CaptureNextPresent(frame, kCaptureTimeoutMs)) return false;
  image.width = frame.width;
  image.height = frame.height;
  image.stride = size_t(frame.width) * 4;
  image.data = std::move(frame.rgba);
  return true;
}

}  // namespace mgrn
