#pragma once

#include <filesystem>
#include <string>

namespace rex::ui {
class Window;
}

// Applies the MGR settings category to the runtime/GPU cvars. Call once from
// ReXApp::OnPreSetup: after the config loads, before the GPU plugin starts.
void MgrApplySettings();
// UI thread only. Resolves "display" to the actual backing-pixel dimensions.
void MgrApplyOutputResolution(rex::ui::Window* window);
int MgrActiveResolutionScale();
struct MgrResolutionStatus {
  int requested = 0;
  int active = 0;  // No initialized renderer; never label this as 720p.
  std::string state = "requested";
  std::string reason;
};
MgrResolutionStatus MgrGetResolutionStatus();
std::string MgrActiveRenderTargetPrecision();
// Saves user choices only: status cvars, per-launch values and the GPU cvars
// that MgrApplySettings derives at startup are left out of the file.
void MgrSaveConfig(const std::filesystem::path& path);
