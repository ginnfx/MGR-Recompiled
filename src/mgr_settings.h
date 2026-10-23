#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rex::ui {
class Window;
}

// Applies the MGR settings category to the runtime/GPU cvars. Call once from
// ReXApp::OnPreSetup: after the config loads, before the GPU plugin starts.
void MgrApplySettings();
// UI thread only. Resolves "display" to the actual backing-pixel dimensions.
void MgrApplyOutputResolution(rex::ui::Window* window);
// Scales in the mgr_resolution_scale form: "1", "1.5", "2" or "3".
struct MgrResolutionStatus {
  std::string requested = "0";
  std::string active = "0";  // No initialized renderer; never label this as 720p.
  std::string state = "requested";
  std::string reason;
};
MgrResolutionStatus MgrGetResolutionStatus();
// Sets the model LOD bias used by the guest chooser hook (mgr_lod.cpp).
void MgrApplyLod(std::string_view value);
std::string MgrActiveRenderTargetPrecision();
// Saves user choices only: status cvars, per-launch values and the GPU cvars
// that MgrApplySettings derives at startup are left out of the file.
void MgrSaveConfig(const std::filesystem::path& path);
