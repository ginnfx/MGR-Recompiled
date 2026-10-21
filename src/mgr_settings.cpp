// MGR options for the in-game settings overlay (F4, "MGR" category).
// Each option is a curated front for runtime/GPU cvars. Internal scale changes
// are sent to the Vulkan command worker for a resource rebuild; target
// precision still reads at cache creation and needs a restart.
#include "mgr_settings.h"

#include "mgr_timing.h"

#include <rex/cvar.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/ui/window.h>

#include <array>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

REXCVAR_DEFINE_STRING(mgr_frame_rate, "60", "MGR/Display",
                      "Frame rate: 60 (original), 120 or unlocked (fractional gameplay "
                      "timing on a 120/240 Hz guest refresh). Applies immediately")
    .allowed({"60", "120", "unlocked"});
REXCVAR_DEFINE_STRING(mgr_output_resolution, "source", "MGR/Display",
                      "Live output image size: source, display (match window/display pixels), "
                      "or WIDTHxHEIGHT. Internal rendering scale is separate")
    .validator([](std::string_view value) {
      int32_t width = 0, height = 0;
      return value == "source" || value == "display" ||
             (rex::graphics::video_mode_util::TryParseResolutionPreset(value, width, height) &&
              width >= 64 && height >= 64 && width <= 8192 && height <= 8192);
    });
REXCVAR_DEFINE_INT32(mgr_resolution_scale, 1, "MGR/Graphics",
                     "Internal resolution: 1 = 1280x720 (original), 2 = 2560x1440, "
                     "3 = 3840x2160. Higher costs GPU time")
    .range(1, 3);
REXCVAR_DEFINE_INT32(mgr_resolution_active_scale, 0, "MGR/Graphics",
                     "Active internal render scale managed by the Vulkan backend (0 = unavailable)")
    .range(0, 3);
REXCVAR_DEFINE_STRING(mgr_resolution_status, "0|requested|0|Waiting for Vulkan initialization",
                      "MGR/Graphics",
                      "Backend status snapshot: request|state|active scale|reason");
REXCVAR_DEFINE_STRING(mgr_texture_filtering, "game", "MGR/Graphics",
                      "Anisotropic filtering: game (as requested by the Xbox 360 game), off, 2x, 4x, "
                      "8x, 16x")
    .allowed({"game", "off", "2x", "4x", "8x", "16x"});
REXCVAR_DEFINE_STRING(mgr_render_target_precision, "high", "MGR/Graphics",
                      "Gamma render targets: high (16-bit, matches Xbox 360 gamma) or fast "
                      "(8-bit sRGB, half the bandwidth; slight banding possible)")
    .allowed({"high", "fast"})
    // The render target cache reads it once, when it is created.
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
using Mapping = std::pair<std::string_view, std::string_view>;
// Highest guest refresh the runtime accepts (video_mode_refresh_rate range).
constexpr uint32_t kUnlockedRefreshHz = 240;
std::string active_render_target_precision = "high";

// Xenos AnisoFilter: -1 keeps the game's value, 0 disabled, 2..5 = 2x..16x.
constexpr std::array<Mapping, 6> kFiltering = {{
    {"game", "-1"}, {"off", "0"}, {"2x", "2"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"},
}};

// GPU-plugin cvars register when the plugin loads, after OnPreSetup. The
// runtime replays REX_<NAME> environment values on late registration, so
// unregistered cvars are handed over that way; registered ones are set directly.
void SetCvar(std::string_view name, std::string_view value) {
  if (rex::cvar::GetFlagInfo(name)) {
    rex::cvar::SetFlagByName(name, value);
    return;
  }
  std::string env = "REX_";
  for (char c : name) env += char(std::toupper(static_cast<unsigned char>(c)));
  setenv(env.c_str(), std::string(value).c_str(), 1);
}

std::string_view Lookup(std::span<const Mapping> table, std::string_view key) {
  for (const auto& [from, to] : table) {
    if (from == key) return to;
  }
  return table.front().second;
}

void ApplyFiltering(std::string_view value) {
  SetCvar("anisotropic_override", Lookup(kFiltering, value));
}

void ApplyPrecision(std::string_view value) {
  SetCvar("gamma_render_target_as_unorm16", value == "fast" ? "false" : "true");
}

// The guest vblank worker and the timing hooks both read their inputs live.
void ApplyFrameRate(std::string_view value) {
  static bool overrides_active = false;
  static std::string saved_vsync;
  static std::string saved_immediate;
  const bool unlocked = value == "unlocked";
  const uint32_t hz = unlocked ? kUnlockedRefreshHz : value == "120" ? 120 : 60;
  SetCvar("video_mode_refresh_rate", std::to_string(hz));
  if (hz != 60) {
    if (!overrides_active) {
      saved_vsync = rex::cvar::GetFlagByName("vsync");
      saved_immediate = rex::cvar::GetFlagByName("vulkan_allow_present_mode_immediate");
      overrides_active = true;
    }
    SetCvar("vsync", "true");  // guest vblank stays a real retrace signal
    SetCvar("vulkan_allow_present_mode_immediate", "true");
  } else if (overrides_active) {
    // Do not overwrite a setting the user changed independently in F4.
    if (!saved_vsync.empty() && rex::cvar::GetFlagByName("vsync") == "true") {
      SetCvar("vsync", saved_vsync);
    }
    if (!saved_immediate.empty() &&
        rex::cvar::GetFlagByName("vulkan_allow_present_mode_immediate") == "true") {
      SetCvar("vulkan_allow_present_mode_immediate", saved_immediate);
    }
    overrides_active = false;
  }
  MgrTimingSetMode(hz == 60    ? MgrTimingMode::kOriginal
                   : unlocked ? MgrTimingMode::kUnlocked
                              : MgrTimingMode::kCap120,
                   hz);
}

void ApplyStartupOptions() {
  // A request is not evidence that Vulkan has created resources at this scale.
  const std::string scale = std::to_string(REXCVAR_GET(mgr_resolution_scale));
  rex::cvar::SetFlagByName("mgr_resolution_active_scale", "0");
  rex::cvar::SetFlagByName("mgr_resolution_status",
                         scale + "|requested|0|Waiting for Vulkan initialization");
  SetCvar("draw_resolution_scale_x", scale);
  SetCvar("draw_resolution_scale_y", scale);
}
}  // namespace

void MgrApplyOutputResolution(rex::ui::Window* window) {
  std::string value = rex::cvar::GetFlagByName("mgr_output_resolution");
  if (value == "display") {
    if (!window || !window->GetActualPhysicalWidth() || !window->GetActualPhysicalHeight()) {
      value = "source";  // Reapplied when the window has a drawable pixel size.
    } else {
      value = std::to_string(std::clamp(window->GetActualPhysicalWidth(), 64u, 8192u)) + "x" +
              std::to_string(std::clamp(window->GetActualPhysicalHeight(), 64u, 8192u));
    }
  }
  // Avoid environment writes or cvar notifications for unchanged resize events.
  if (rex::cvar::GetFlagByName("vulkan_output_resolution") != value) {
    SetCvar("vulkan_output_resolution", value);
  }
}

void MgrApplySettings() {
  // Import the old launcher environment as a startup choice, not a permanent
  // override: the live options must still be able to return to original timing.
  const char* legacy_fps = std::getenv("MGR_EXPERIMENTAL_FPS");
  if (legacy_fps && std::string_view(legacy_fps) == "120" &&
      REXCVAR_GET(mgr_frame_rate) == "60") {
    rex::cvar::SetFlagByName("mgr_frame_rate", "120");
  }
  ApplyStartupOptions();
  active_render_target_precision = REXCVAR_GET(mgr_render_target_precision);
  MgrApplyOutputResolution(nullptr);
  ApplyFrameRate(REXCVAR_GET(mgr_frame_rate));
  ApplyFiltering(REXCVAR_GET(mgr_texture_filtering));
  ApplyPrecision(REXCVAR_GET(mgr_render_target_precision));
  // Startup mirroring is not a user change awaiting a restart.
  rex::cvar::ClearPendingRestartFlags();
  rex::cvar::RegisterChangeCallback(
      "mgr_frame_rate", [](std::string_view, std::string_view value) { ApplyFrameRate(value); });
  rex::cvar::RegisterChangeCallback(
      "mgr_texture_filtering", [](std::string_view, std::string_view value) { ApplyFiltering(value); });
  rex::cvar::RegisterChangeCallback(
      "mgr_render_target_precision",
      [](std::string_view, std::string_view value) { ApplyPrecision(value); });
}

MgrResolutionStatus MgrGetResolutionStatus() {
  MgrResolutionStatus result;
  // One locked cvar read avoids mixing fields from different GPU transitions.
  const std::string snapshot = rex::cvar::GetFlagByName("mgr_resolution_status");
  const auto first = snapshot.find('|');
  const auto second = first == std::string::npos ? first : snapshot.find('|', first + 1);
  const auto third = second == std::string::npos ? second : snapshot.find('|', second + 1);
  if (third == std::string::npos) return result;
  result.requested = std::atoi(snapshot.substr(0, first).c_str());
  result.state = snapshot.substr(first + 1, second - first - 1);
  result.active = std::atoi(snapshot.substr(second + 1, third - second - 1).c_str());
  result.reason = snapshot.substr(third + 1);
  return result;
}

int MgrActiveResolutionScale() { return MgrGetResolutionStatus().active; }

std::string MgrActiveRenderTargetPrecision() { return active_render_target_precision; }

void MgrSaveConfig(const std::filesystem::path& path) {
  static constexpr std::string_view kNotSaved[] = {
      // Backend-owned status and per-launch values.
      "imgui_guest_paced", "mgr_resolution_status", "mgr_resolution_active_scale", "log_file",
      // Derived from mgr_* settings by MgrApplySettings on every launch.
      "video_mode_refresh_rate", "anisotropic_override", "gamma_render_target_as_unorm16", "draw_resolution_scale_x",
      "draw_resolution_scale_y"};
  rex::cvar::SaveConfig(path);
  std::ifstream in(path);
  if (!in) return;
  std::ostringstream kept;
  for (std::string line; std::getline(in, line);) {
    const std::string_view key = std::string_view(line).substr(0, line.find(" ="));
    if (std::find(std::begin(kNotSaved), std::end(kNotSaved), key) == std::end(kNotSaved))
      kept << line << '\n';
  }
  in.close();
  std::ofstream(path, std::ios::trunc) << kept.str();
}
