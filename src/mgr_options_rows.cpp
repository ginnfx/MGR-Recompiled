#include "mgr_options_rows.h"

#include "native/mgr_native.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string_view>
#include <vector>

namespace mgr_options_rows {
namespace {
// MGR_OPTIONS_TEST_ROWS upper bound.
constexpr long kMaxTestRows = 64;
constexpr Choice kScaleChoices[] = {{"1280X720", "1"}, {"1920X1080", "1.5"}, {"2560X1440", "2"}, {"3840X2160", "3"}};
constexpr Choice kBool[] = {{"ON", "true"}, {"OFF", "false"}};
constexpr Choice kRate[] = {{"60 FPS", "60"}, {"120 FPS (EXPERIMENTAL)", "120"}, {"UNLOCKED (EXPERIMENTAL)", "unlocked"}};
constexpr Choice kPacing[] = {{"LOW LATENCY", "true"}, {"COMPATIBILITY", "false"}};
constexpr Choice kFilter[] = {{"GAME DEFAULT", "game"}, {"OFF", "off"}, {"2X", "2x"}, {"4X", "4x"}, {"8X", "8x"}, {"16X", "16x"}};
constexpr Choice kPrecision[] = {{"HIGH (ACCURATE)", "high"}, {"FAST (RESTART REQUIRED)", "fast"}};
constexpr Choice kOutput[] = {{"MATCH GAME", "source"}, {"MATCH DISPLAY", "display"}, {"1920X1080 OUTPUT", "1920x1080"}, {"2560X1440 OUTPUT", "2560x1440"}};
constexpr Choice kLod[] = {{"GAME DEFAULT", "game"}, {"HIGH", "high"}, {"MAXIMUM", "max"}};
constexpr Choice kMsaa[] = {{"OFF", "0"}, {"2X", "2"}, {"4X", "4"}, {"8X", "8"}};
constexpr Choice kUpscaler[] = {{"OFF", "off"}, {"METALFX QUALITY", "metalfx_quality"}, {"METALFX BALANCED", "metalfx_balanced"},
                                {"METALFX PERFORMANCE", "metalfx_performance"}};
constexpr Choice kShadow[] = {{"ORIGINAL (1024)", "0"}, {"2048", "2048"}, {"4096", "4096"}};
constexpr Choice kSharpening[] = {{"OFF", "0"}, {"LOW", "0.3"}, {"MEDIUM", "0.6"}, {"HIGH", "1"}};
constexpr Choice kBrightness[] = {{"0.7", "0.7"}, {"0.8", "0.8"}, {"0.9", "0.9"}, {"1.0 (DEFAULT)", "1"},
                                  {"1.1", "1.1"}, {"1.2", "1.2"}, {"1.3", "1.3"}};
}  // namespace

const std::span<const Choice> kScale = kScaleChoices;
const Option kInternalResolution = {"mgr_resolution_scale", "INTERNAL RESOLUTION", "Changes internal rendering detail. Applies after the GPU rebuilds its resources.", kScaleChoices};

namespace {
// Every graphics, video and screen option, in one scrolling page.
const Option kVideo[] = {
  {"fullscreen", "FULLSCREEN", "Uses the current display in fullscreen mode.", kBool},
  {"mgr_output_resolution", "OUTPUT RESOLUTION", "Resizes the completed image. Internal rendering detail is controlled separately.", kOutput},
  kInternalResolution,
  {"mgr_upscaler", "UPSCALING", "Renders below the internal resolution and rebuilds the detail with MetalFX. Turns anti-aliasing off.", kUpscaler, Renderer::Native},
  {"mgr_msaa", "ANTI-ALIASING", "Multisampling smooths jagged edges. Higher settings cost frame rate.", kMsaa, Renderer::Native},
  {"mgr_transparency_aa", "TRANSPARENCY AA", "Smooths the edges of hair, fences and foliage. Needs anti-aliasing.", kBool, Renderer::Native},
  {"mgr_shadow_resolution", "SHADOW RESOLUTION", "Sharper, more detailed shadows. Higher settings use more video memory.", kShadow, Renderer::Native},
  {"present_letterbox", "PRESERVE ASPECT RATIO", "Preserves the original image proportions on wider or taller displays.", kBool},
  {"mgr_vsync", "VSYNC", "Syncs frames to the display refresh, preventing tearing. Frame rates above the refresh rate need it off.", kBool},
  {"mgr_frame_rate", "FRAME RATE", "60 FPS is the original timing. Higher rates are experimental.", kRate},
  {"vulkan_nonblocking_guest_present", "FRAME PACING", "Low latency avoids blocking the game while waiting for a display image.", kPacing, Renderer::Xenos},
  {"mgr_brightness", "BRIGHTNESS", "Brightens or darkens the image. 1.0 is the original look.", kBrightness, Renderer::Native},
  {"mgr_sharpening", "SHARPENING", "Restores fine detail softened by upscaling or anti-aliasing.", kSharpening, Renderer::Native},
  {"mgr_texture_filtering", "TEXTURE FILTERING", "Anisotropic filtering improves textures viewed at an angle.", kFilter},
  {"mgr_lod", "MODEL DETAIL", "Detail and draw distance of far away characters and objects. Higher settings cost frame rate.", kLod},
  {"mgr_render_target_precision", "RENDERING QUALITY", "High preserves the original gamma precision. Changing this setting requires a restart.", kPrecision, Renderer::Xenos},
  {"mgr_bloom", "BLOOM", "The glow around lights, sparks and Blade Mode effects.", kBool},
  {"mgr_film_grain", "FILM GRAIN", "Enables the original film grain post processing effect.", kBool},
};

constexpr Choice kPrompts[] = {{"AUTOMATIC", "auto"}, {"KEYBOARD", "keyboard"}, {"XBOX", "xbox"}, {"PLAYSTATION", "playstation"}};
constexpr Choice kSensitivity[] = {{"0.5", "0.5"}, {"1.0", "1"}, {"2.0", "2"}, {"3.0", "3"}};
const Option kMisc[] = {
  {"mgr_prompt_style", "BUTTON PROMPTS", "Automatic follows the keyboard or controller used last.", kPrompts},
  {"mnk_mouse", "MOUSE CAMERA", "Use the mouse for camera movement and free angle Blade Mode.", kBool},
  {"mnk_sensitivity", "MOUSE SENSITIVITY", "Changes mouse camera and Blade Mode sensitivity.", kSensitivity},
  {"mgr_skip_intro_logos", "SKIP INTRO LOGOS", "Skips any remaining boot logos. Applies to the next launch.", kBool},
};

// MSAA levels the GPU cannot run are left out.
std::span<const Choice> SupportedMsaa() {
  static const std::vector<Choice> shown = [] {
    std::vector<Choice> levels;
    std::copy_if(std::begin(kMsaa), std::end(kMsaa), std::back_inserter(levels),
                 [](const Choice& c) { return std::atoi(c.value) <= mgrn::MgrNativeMaxMSAA(); });
    return levels;
  }();
  return shown;
}
// The rows of a page that apply to this run's renderer (fixed at launch).
std::vector<Option> ForRenderer(std::span<const Option> all) {
  const Renderer active = mgrn::NativeRendererActive() ? Renderer::Native : Renderer::Xenos;
  std::vector<Option> shown;
  for (Option option : all) {
    if (option.renderer != Renderer::Any && option.renderer != active) continue;
    if (std::string_view(option.key) == "mgr_msaa") option.choices = SupportedMsaa();
    shown.push_back(option);
  }
  // MGR_OPTIONS_TEST_ROWS=<n> (diagnostic): repeat the rows up to n, to exercise scrolling.
  static const size_t wanted = [] {
    const char* text = std::getenv("MGR_OPTIONS_TEST_ROWS");
    return text ? size_t(std::clamp(std::strtol(text, nullptr, 10), 0L, kMaxTestRows)) : size_t(0);
  }();
  for (size_t i = 0; wanted && !shown.empty() && shown.size() < wanted; ++i) shown.push_back(shown[i]);
  return shown;
}
}  // namespace

std::span<const Option> VideoRows() {
  static const std::vector<Option> rows = ForRenderer(kVideo);
  return rows;
}
std::span<const Option> MiscRows() {
  static const std::vector<Option> rows = ForRenderer(kMisc);
  return rows;
}

}  // namespace mgr_options_rows
