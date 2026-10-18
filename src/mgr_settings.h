#pragma once

namespace rex::ui {
class Window;
}

// Applies the MGR settings category to the runtime/GPU cvars. Call once from
// ReXApp::OnPreSetup: after the config loads, before the GPU plugin starts.
void MgrApplySettings();
// UI thread only. Resolves "display" to the actual backing-pixel dimensions.
void MgrApplyOutputResolution(rex::ui::Window* window);
