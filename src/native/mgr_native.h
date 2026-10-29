// MGR native renderer entry points used by the app (src/mgr_app.h).
//
// mgr_renderer = "native" swaps the Xenos GPU emulator for the D3D-hook
// renderer: the guest GPU services run on rexgpu-mgrnull (vblank, fences,
// interrupts), and the hooks in src/native/hooks draw through plume.
#pragma once

#include <memory>

namespace rex {
struct RuntimeConfig;
}
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
class WindowedAppContext;
struct RawImage;
}

namespace mgrn {

// True when this run uses the native renderer. Fixed by MgrNativeConfigure,
// before any guest code runs; every hook falls through to the guest original
// while it is false.
bool NativeRendererActive();

// OnPreSetup: reads mgr_renderer and, when native, selects rexgpu-mgrnull.
void MgrNativeConfigure(rex::RuntimeConfig& config);

// OnPreLaunchModule: creates the plume device and swapchain on the window.
// False (logged) leaves the run without a renderer. Safe to call again once
// the first-run setup has already brought the device up.
bool MgrNativeStart(rex::ui::Window* window);

// ImGui drawer on the plume device, for the SDK's detached overlay mode. It
// binds to the device lazily, so it can be made before MgrNativeStart.
std::unique_ptr<rex::ui::ImmediateDrawer> MgrNativeCreateOverlayDrawer();

// First-run setup before the runtime exists: starts the device and presents
// frames that only draw the ImGui dialogs, until MgrNativeStopOverlayOnly.
bool MgrNativeStartOverlayOnly(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
                               rex::ui::WindowedAppContext& context);
void MgrNativeStopOverlayOnly();

// Highest MSAA sample count the device supports for the scene targets; 1
// before the device is up.
int MgrNativeMaxMSAA();

// Test harness capture: the next presented frame as R8G8B8X8. False when no
// frame was presented within the timeout.
bool MgrNativeCapture(rex::ui::RawImage& image);

}  // namespace mgrn
