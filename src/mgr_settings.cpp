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
#include <vector>

REXCVAR_DEFINE_STRING(mgr_frame_rate, "60", "MGR/Display",
                      "Frame rate: 60 (original), 120 or unlocked (fractional gameplay "
                      "timing on a 120/240 Hz guest refresh). Applies immediately")
    .allowed({"60", "120", "unlocked"});
// The SDK's `vsync` cvar is the guest retrace clock, not display sync: the
// presenter picks IMMEDIATE (tearing) whenever it is allowed. This is the real
// display option; it maps to the swapchain present mode (FIFO when on).
REXCVAR_DEFINE_BOOL(mgr_vsync, true, "MGR/Display",
                    "Display VSync: present in step with the display refresh (no tearing). "
                    "Off allows tearing for lower latency. Applies immediately");
REXCVAR_DEFINE_STRING(mgr_output_resolution, "source", "MGR/Display",
                      "Live output image size: source, display (match window/display pixels), "
                      "or WIDTHxHEIGHT. Internal rendering scale is separate")
    .validator([](std::string_view value) {
      int32_t width = 0, height = 0;
      return value == "source" || value == "display" ||
             (rex::graphics::video_mode_util::TryParseResolutionPreset(value, width, height) &&
              width >= 64 && height >= 64 && width <= 8192 && height <= 8192);
    });
namespace {
// mgr_resolution_scale in its saved form ("1", "1.5", "2", "3"), also accepting
// float spellings such as "1.500000" (an unquoted TOML float). Empty if
// unsupported. Mirrors ParseScaleRatio in the patched SDK's scale_ratio.h,
// which the installed SDK headers used here do not ship.
std::string CanonicalResolutionScale(std::string_view value) {
  const std::string text(value);
  char* end = nullptr;
  const double scale = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') return {};
  if (scale == 1.0) return "1";
  if (scale == 1.5) return "1.5";
  if (scale == 2.0) return "2";
  if (scale == 3.0) return "3";
  return {};
}
}  // namespace

// A string so that 1.5 fits. Integer values saved by older builds load as is;
// an unquoted TOML float arrives as "1.500000" and is normalized at startup.
REXCVAR_DEFINE_STRING(mgr_resolution_scale, "1", "MGR/Graphics",
                      "Internal resolution: 1 = 1280x720 (original), 1.5 = 1920x1080, "
                      "2 = 2560x1440, 3 = 3840x2160. Higher costs GPU time")
    .validator([](std::string_view value) { return !CanonicalResolutionScale(value).empty(); });
REXCVAR_DEFINE_STRING(mgr_resolution_active_scale, "0", "MGR/Graphics",
                      "Active internal render scale managed by the Vulkan backend (0 = unavailable)");
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

// MoltenVK offers only FIFO and IMMEDIATE; the presenter reads this cvar when it
// creates a swapchain (MgrApp recreates it on change).
void ApplyDisplaySync(bool on) {
  SetCvar("vulkan_allow_present_mode_immediate", on ? "false" : "true");
}

// The guest vblank worker and the timing hooks both read their inputs live.
void ApplyFrameRate(std::string_view value) {
  static bool overrides_active = false;
  static std::string saved_vsync;
  const bool unlocked = value == "unlocked";
  const uint32_t hz = unlocked ? kUnlockedRefreshHz : value == "120" ? 120 : 60;
  SetCvar("video_mode_refresh_rate", std::to_string(hz));
  if (hz != 60) {
    if (!overrides_active) {
      saved_vsync = rex::cvar::GetFlagByName("vsync");
      overrides_active = true;
    }
    SetCvar("vsync", "true");  // guest vblank stays a real retrace signal
  } else if (overrides_active) {
    // Do not overwrite a setting the user changed independently in F4.
    if (!saved_vsync.empty() && rex::cvar::GetFlagByName("vsync") == "true") {
      SetCvar("vsync", saved_vsync);
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
  std::string scale = CanonicalResolutionScale(REXCVAR_GET(mgr_resolution_scale));
  if (scale.empty()) scale = "1";
  if (scale != REXCVAR_GET(mgr_resolution_scale))
    rex::cvar::SetFlagByName("mgr_resolution_scale", scale);
  rex::cvar::SetFlagByName("mgr_resolution_active_scale", "0");
  rex::cvar::SetFlagByName("mgr_resolution_status",
                         scale + "|requested|0|Waiting for Vulkan initialization");
  // 1.5 is scale 3 over denominator 2 in the GPU cvars.
  const bool three_halves = scale == "1.5";
  SetCvar("draw_resolution_scale_x", three_halves ? "3" : scale);
  SetCvar("draw_resolution_scale_y", three_halves ? "3" : scale);
  SetCvar("draw_resolution_scale_denominator", three_halves ? "2" : "1");
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
  ApplyDisplaySync(REXCVAR_GET(mgr_vsync));
  ApplyFiltering(REXCVAR_GET(mgr_texture_filtering));
  ApplyPrecision(REXCVAR_GET(mgr_render_target_precision));
  // Startup mirroring is not a user change awaiting a restart.
  rex::cvar::ClearPendingRestartFlags();
  rex::cvar::RegisterChangeCallback(
      "mgr_frame_rate", [](std::string_view, std::string_view value) { ApplyFrameRate(value); });
  rex::cvar::RegisterChangeCallback(
      "mgr_vsync", [](std::string_view, std::string_view value) { ApplyDisplaySync(value == "true"); });
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
  result.requested = snapshot.substr(0, first);
  result.state = snapshot.substr(first + 1, second - first - 1);
  result.active = snapshot.substr(second + 1, third - second - 1);
  result.reason = snapshot.substr(third + 1);
  return result;
}

std::string MgrActiveRenderTargetPrecision() { return active_render_target_precision; }

namespace {

std::string_view ConfigKey(std::string_view line) { return line.substr(0, line.find(" =")); }

}  // namespace

void MgrSaveConfig(const std::filesystem::path& path) {
  static constexpr std::string_view kNeverSaved[] = {
      // Backend-owned status.
      "imgui_guest_paced", "mnk_menu_navigation", "mgr_resolution_status",
      "mgr_resolution_active_scale",
      // Derived from mgr_* settings by MgrApplySettings on every launch.
      "video_mode_refresh_rate", "vulkan_allow_present_mode_immediate", "anisotropic_override", "gamma_render_target_as_unorm16",
      "draw_resolution_scale_x", "draw_resolution_scale_y", "draw_resolution_scale_denominator"};
  // Often passed on the command line (scripts/run.sh): the file keeps the line
  // the user wrote, if any, instead of this launch's value.
  static constexpr std::string_view kPerLaunch[] = {"log_file"};
  // Experiments (mgr_exp_*) are opt-in per launch and never persisted.
  static constexpr std::string_view kExperimentPrefix = "mgr_exp_";
  const auto listed = [](const auto& keys, std::string_view key) {
    return std::find(std::begin(keys), std::end(keys), key) != std::end(keys);
  };
  std::vector<std::string> user_lines;
  if (std::ifstream before(path); before) {
    for (std::string line; std::getline(before, line);)
      if (listed(kPerLaunch, ConfigKey(line))) user_lines.push_back(line);
  }
  rex::cvar::SaveConfig(path);
  std::ifstream in(path);
  if (!in) return;
  std::ostringstream kept;
  for (std::string line; std::getline(in, line);) {
    const std::string_view key = ConfigKey(line);
    if (listed(kNeverSaved, key) || key.starts_with(kExperimentPrefix)) continue;
    if (listed(kPerLaunch, key)) {
      const auto user = std::find_if(user_lines.begin(), user_lines.end(),
                                     [&](const std::string& u) { return ConfigKey(u) == key; });
      if (user != user_lines.end()) kept << *user << '\n';
      continue;
    }
    kept << line << '\n';
  }
  in.close();
  std::ofstream(path, std::ios::trunc) << kept.str();
}
