#include "mgr_native.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

#include "core/logging.h"
#include "gpu/capture.h"
#include "gpu/device.h"
#include "gpu/imgui_overlay_drawer.h"
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
constexpr auto kOverlayFrameInterval = std::chrono::milliseconds(16);
bool g_device_started = false;  // UI thread only
constexpr int kSetupCaptures = 6;
constexpr auto kSetupCaptureInterval = std::chrono::seconds(3);

void WritePpm(const std::string& path, const gpu::CapturedFrame& frame) {
  std::ofstream file(path, std::ios::binary);
  file << "P6\n" << frame.width << " " << frame.height << "\n255\n";
  for (size_t i = 0; i + 3 < frame.rgba.size(); i += 4)
    file.write(reinterpret_cast<const char*>(frame.rgba.data() + i), 3);
}

// Overlay-only presents run on the UI thread; the ticks come from this thread
// so a re-queued UI task never starves SDL's event handling.
std::thread g_overlay_ticker;
std::atomic<bool> g_overlay_stop{false};
std::atomic<bool> g_overlay_tick_pending{false};
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
  if (!NativeRendererActive() || g_device_started) return true;
  if (!platform::PrepareVulkanLoader()) return false;
  if (!gpu::Video::CreateHostDevice(window)) {
    MGRN_ERROR("Native renderer: device creation failed");
    return false;
  }
  MGRN_INFO("Native renderer ready: {} on {}", gpu::Video::GetBackendInfo(),
            gpu::Video::GetDeviceName());
  g_device_started = true;
  return true;
}

std::unique_ptr<rex::ui::ImmediateDrawer> MgrNativeCreateOverlayDrawer() {
  return std::make_unique<gpu::ImGuiOverlayDrawer>();
}

bool MgrNativeStartOverlayOnly(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
                               rex::ui::WindowedAppContext& context) {
  if (!NativeRendererActive() || !drawer || !MgrNativeStart(window)) return false;
  gpu::Video::SetOverlayDrawHook(
      [drawer](plume::RenderCommandList* cmd, plume::RenderFramebuffer* fb, u32 w, u32 h) {
        gpu::OverlayUIDrawContext ctx(w, h, cmd, fb);
        drawer->Draw(ctx);
      });
  g_overlay_stop = false;
  // Diagnostic: MGR_SETUP_CAPTURE_DIR=<dir> writes setup-<n>.ppm every few
  // seconds, so hidden-window runs can check the setup screen.
  const char* capture_dir = std::getenv("MGR_SETUP_CAPTURE_DIR");
  if (capture_dir && *capture_dir) {
    std::thread([dir = std::string(capture_dir)] {
      for (int n = 0; n < kSetupCaptures && !g_overlay_stop.load(); ++n) {
        std::this_thread::sleep_for(kSetupCaptureInterval);
        gpu::CapturedFrame frame;
        if (!gpu::CaptureNextPresent(frame, kCaptureTimeoutMs)) continue;
        WritePpm(dir + "/setup-" + std::to_string(n) + ".ppm", frame);
      }
    }).detach();
  }
  g_overlay_ticker = std::thread([&context] {
    while (!g_overlay_stop.load(std::memory_order_acquire)) {
      if (!g_overlay_tick_pending.exchange(true, std::memory_order_acq_rel)) {
        context.CallInUIThreadDeferred([] {
          gpu::Video::PresentOverlayFrame();
          g_overlay_tick_pending.store(false, std::memory_order_release);
        });
      }
      std::this_thread::sleep_for(kOverlayFrameInterval);
    }
  });
  return true;
}

void MgrNativeStopOverlayOnly() {
  g_overlay_stop = true;
  if (g_overlay_ticker.joinable()) g_overlay_ticker.join();
  gpu::Video::SetOverlayDrawHook(nullptr);
}

int MgrNativeMaxMSAA() {
  const auto mask = gpu::state().supported_sample_mask;
  int samples = 1;
  for (int count = 2; count <= 8; count *= 2)
    if (mask & u32(count)) samples = count;
  return samples;
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
