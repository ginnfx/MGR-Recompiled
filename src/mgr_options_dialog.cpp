#include "mgr_options_dialog.h"

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/ui/window.h>

#include <imgui.h>

#include <algorithm>
#include <span>
#include <string>

namespace {
struct Choice {
  const char* label;
  const char* value;
};
struct Option {
  const char* cvar;
  const char* label;
  const char* help;
  std::span<const Choice> choices;
  bool restart;
};

constexpr Choice kOnOff[] = {{"On", "true"}, {"Off", "false"}};
constexpr Choice kFrameRate[] = {{"60 FPS (original)", "60"},
                                 {"120 FPS (experimental)", "120"},
                                 {"Unlocked (240 Hz ceiling)", "unlocked"}};
constexpr Choice kResolution[] = {
    {"1280x720 (original)", "1"}, {"2560x1440", "2"}, {"3840x2160", "3"}};
constexpr Choice kFiltering[] = {{"Game default", "game"}, {"Off", "off"},
                                 {"2x", "2x"}, {"4x", "4x"}, {"8x", "8x"}, {"16x", "16x"}};
constexpr Choice kPrecision[] = {{"High (accurate)", "high"}, {"Fast (less bandwidth)", "fast"}};
constexpr Choice kPrompts[] = {{"Automatic (last used device)", "auto"},
                               {"Keyboard", "keyboard"},
                               {"Xbox", "xbox"},
                               {"PlayStation", "playstation"}};

constexpr Option kDisplay[] = {
    {"fullscreen", "Fullscreen", "Fullscreen follows the current display's pixel size.",
     kOnOff, false},
    {"present_letterbox", "Preserve aspect ratio",
     "Keep the original image proportions with black bars on a 16:10 or ultrawide display. "
     "Off stretches the image to fill the window.", kOnOff, false},
    {"mgr_frame_rate", "Frame rate",
     "Above 60 is experimental: physics, cutscenes, Blade Mode and QTEs need verification. "
     "Unlocked removes the host cap, with a 240 Hz guest refresh ceiling.",
     kFrameRate, false},
};
constexpr Option kGraphics[] = {
    {"mgr_resolution_scale", "Internal rendering scale",
     "The current renderer supports integer scales of 720p and reads this at startup. "
     "Output size is independent and can be changed live.", kResolution, true},
    {"mgr_texture_filtering", "Texture filtering",
     "Anisotropic filtering. Game default preserves the game's own requests.", kFiltering,
     false},
    {"mgr_render_target_precision", "Render target precision",
     "Fast uses 8-bit sRGB gamma targets: half the bandwidth, slight banding possible.",
     kPrecision, true},
    {"mgr_film_grain", "Film grain", "The noise layer of the final post-process filter.", kOnOff,
     false},
};
constexpr Option kGame[] = {
    {"mgr_skip_intro_logos", "Skip intro logos",
     "Skips any remaining boot logos. Already-finished logos are unaffected.", kOnOff, false},
};
constexpr Option kControls[] = {
    {"mgr_prompt_style", "Button prompts",
     "Automatic follows the keyboard or controller you used last.", kPrompts, false},
    {"mnk_mouse", "Mouse camera / Blade Mode",
     "Use the mouse as the right stick for camera movement and free-angle Blade Mode. "
     "Mouse capture follows window focus; arrow-key right-stick input still works.",
     kOnOff, false},
};

bool IsPendingRestart(const char* cvar) {
  const auto pending = rex::cvar::GetPendingRestartFlags();
  return std::find(pending.begin(), pending.end(), cvar) != pending.end();
}

bool DrawOption(const Option& option) {
  bool changed = false;
  const std::string current = rex::cvar::GetFlagByName(option.cvar);
  const Choice* selected = &option.choices.front();
  for (const Choice& choice : option.choices) {
    if (current == choice.value) selected = &choice;
  }
  ImGui::PushID(option.cvar);
  ImGui::TextUnformatted(option.label);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.help);
  ImGui::SameLine(220.0f);
  ImGui::SetNextItemWidth(260.0f);
  if (ImGui::BeginCombo("##v", selected->label)) {
    for (const Choice& choice : option.choices) {
      if (ImGui::Selectable(choice.label, &choice == selected)) {
        rex::cvar::SetFlagByName(option.cvar, choice.value);
        changed = current != choice.value;
      }
    }
    ImGui::EndCombo();
  }
  if (option.restart && IsPendingRestart(option.cvar)) {
    ImGui::SameLine();
    ImGui::TextDisabled("(after restart)");
  }
  ImGui::PopID();
  return changed;
}

bool DrawSection(const char* title, std::span<const Option> options) {
  bool changed = false;
  ImGui::SeparatorText(title);
  for (const Option& option : options) changed |= DrawOption(option);
  return changed;
}
}  // namespace

MgrOptionsDialog::MgrOptionsDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                                   std::filesystem::path config_path, rex::ui::Window* window)
    : ImGuiDialog(drawer), input_(input), window_(window), config_path_(std::move(config_path)) {
  if (window_) {
    output_width_ = int(window_->GetActualPhysicalWidth());
    output_height_ = int(window_->GetActualPhysicalHeight());
  }
  if (input_) input_->AddUIInputBlocker();
}

bool MgrOptionsDialog::DrawOutputResolution() {
  const std::string current = rex::cvar::GetFlagByName("mgr_output_resolution");
  if (last_output_selection_ != current) {
    last_output_selection_ = current;
    custom_output_ = current != "source" && current != "display";
    if (custom_output_) {
      int32_t width = 0, height = 0;
      if (rex::graphics::video_mode_util::TryParseResolutionPreset(current, width, height)) {
        output_width_ = width;
        output_height_ = height;
      }
    }
  }
  bool changed = false;
  ImGui::TextUnformatted("Output resolution (live)");
  ImGui::SameLine(220.0f);
  ImGui::SetNextItemWidth(260.0f);
  const char* preview = custom_output_ ? "Custom" : current == "display" ? "Match display"
                                                                          : "Source image";
  if (ImGui::BeginCombo("##output_resolution", preview)) {
    for (const Choice& choice : {Choice{"Source image", "source"},
                                 Choice{"Match display", "display"}}) {
      if (ImGui::Selectable(choice.label, !custom_output_ && current == choice.value)) {
        rex::cvar::SetFlagByName("mgr_output_resolution", choice.value);
        custom_output_ = false;
        changed |= current != choice.value;
      }
    }
    if (ImGui::Selectable("Custom", custom_output_)) custom_output_ = true;
    ImGui::EndCombo();
  }
  if (custom_output_) {
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Width", &output_width_, 1, 64);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("Height", &output_height_, 1, 64);
    const bool valid = output_width_ >= 64 && output_width_ <= 8192 &&
                       output_height_ >= 64 && output_height_ <= 8192;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Apply output size")) {
      const std::string value = std::to_string(output_width_) + "x" + std::to_string(output_height_);
      rex::cvar::SetFlagByName("mgr_output_resolution", value);
      changed |= current != value;
    }
    ImGui::EndDisabled();
    if (!valid) ImGui::TextDisabled("Each dimension must be 64..8192 pixels.");
  }
  ImGui::TextWrapped("Output resizing scales the completed image. Internal rendering quality "
                     "is controlled separately below.");
  if (window_) {
    ImGui::TextDisabled("Window / display output: %u x %u pixels",
                        window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
  }
  const std::string applied = rex::cvar::GetFlagByName("vulkan_output_resolution");
  ImGui::TextDisabled("Selected output: %s", applied.c_str());
  return changed;
}

MgrOptionsDialog::~MgrOptionsDialog() {
  if (input_) input_->RemoveUIInputBlocker();
}

void MgrOptionsDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (!ImGui::Begin("MGR Options (F2)", nullptr,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  bool changed = DrawSection("Display", kDisplay);
  changed |= DrawOutputResolution();
  changed |= DrawSection("Graphics", kGraphics);
  changed |= DrawSection("Game", kGame);
  changed |= DrawSection("Controls", kControls);
  if (changed) saved_ = false;
  ImGui::TextWrapped("Free Blade Mode uses Blade Mode + right-stick direction. "
                     "Keyboard defaults: hold Q and use the arrow keys, or enable mouse input.");
  ImGui::TextDisabled("Rebind keys: Settings (F4) > Input > Keybinds.");

  ImGui::Separator();
  if (ImGui::Button("Save")) {
    rex::cvar::SaveConfig(config_path_);
    saved_ = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled(saved_ ? "Saved to %s" : "Writes %s",
                      config_path_.filename().string().c_str());
  ImGui::End();
}
