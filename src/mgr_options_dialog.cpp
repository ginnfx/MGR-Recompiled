#include "mgr_options_dialog.h"
#include "mgr_game_options.h"
#include "mgr_key_labels.h"
#include "mgr_menu_fonts.h"
#include "mgr_settings.h"

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/ui/window.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

float MgrGuestFPS();  // Guest VdSwap rate; host UI presents can repeat a frame.

namespace {
bool game_style = false;  // UI thread, scoped to the integrated draw below.
float menu_scale = 1;
std::string menu_help;

std::string Uppercase(std::string text) {
  for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

// Native OPTIONS uses a label above its value, with a full-row blue selection
// and left/right changes. ImGui supplies hit testing/focus, never visible chrome.
int MenuRow(const char* id, const char* label, const char* value, const char* help) {
  ImGui::PushID(id);
  const float s = menu_scale;
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = std::min(ImGui::GetContentRegionAvail().x, 610 * s);
  const bool pressed = ImGui::Selectable("##row", false, 0, ImVec2(width, 45 * s));
  const bool active = ImGui::IsItemHovered() || ImGui::IsItemFocused();
  if (active && help) menu_help = help;
  auto* draw = ImGui::GetWindowDrawList();
  if (active) draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + 45 * s),
                                   IM_COL32(40, 96, 119, 220));
  const auto title = Uppercase(label);
  draw->AddText(MgrMenuLabelFont(), 30 * s, ImVec2(pos.x + 5 * s, pos.y - 3 * s),
                active ? IM_COL32(120, 236, 250, 255) : IM_COL32(135, 178, 182, 255), title.c_str());
  const auto text = Uppercase(value);
  draw->AddText(MgrMenuValueFont(), 24 * s, ImVec2(pos.x + 5 * s, pos.y + 20 * s),
                IM_COL32(188, 217, 218, 255), text.c_str());
  const float ax = pos.x + width - 28 * s, ay = pos.y + 24 * s;
  const auto color = active ? IM_COL32(108, 235, 250, 255) : IM_COL32(70, 120, 132, 190);
  draw->AddTriangleFilled(ImVec2(ax - 12*s, ay), ImVec2(ax - 5*s, ay - 5*s),
                          ImVec2(ax - 5*s, ay + 5*s), color);
  draw->AddTriangleFilled(ImVec2(ax + 12*s, ay), ImVec2(ax + 5*s, ay - 5*s),
                          ImVec2(ax + 5*s, ay + 5*s), color);
  int direction = pressed ? 1 : 0;
  // Selectable reports a mouse press on release, so test where the click began.
  const float click_x = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left].x;
  if (pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && click_x > ax - 22 * s &&
      click_x < ax) direction = -1;
  if (active && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) direction = -1;
  if (ImGui::IsItemFocused()) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) direction = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) direction = 1;
  }
  ImGui::PopID();
  return direction;
}

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
    {"1280x720 (original)", "1"}, {"1920x1080", "1.5"}, {"2560x1440", "2"},
    {"3840x2160", "3"}};
constexpr Choice kFiltering[] = {{"Game default", "game"}, {"Off", "off"},
                                 {"2x", "2x"}, {"4x", "4x"}, {"8x", "8x"}, {"16x", "16x"}};
constexpr Choice kPrecision[] = {{"High (accurate)", "high"}, {"Fast (less bandwidth)", "fast"}};
constexpr Choice kLod[] = {{"Game default", "game"}, {"High (2x)", "high"}, {"Maximum", "max"}};
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
     "Requests a Vulkan resource rebuild; check the active scale and failure status. "
     "Internal targets: 720p (original), 1080p (1.5x), 1440p and 2160p. Output image "
     "size is separate and can match the display.",
     kResolution, false},
    {"mgr_texture_filtering", "Texture filtering",
     "Anisotropic filtering. Game default preserves the game's own requests.", kFiltering,
     false},
    {"mgr_lod", "Model detail",
     "Detail of distant characters and objects. High doubles the detail distance and draw "
     "distance; Maximum keeps full detail at any distance and draws four times as far.",
     kLod, false},
    {"mgr_render_target_precision", "Render target precision",
     "Fast uses 8-bit sRGB gamma targets: half the bandwidth, slight banding possible.",
     kPrecision, true},
    {"mgr_film_grain", "Film grain", "The noise layer of the final post-process filter.", kOnOff,
     false},
    {"mgr_bloom", "Bloom glow",
     "The glare blur around lights and effects. Off skips about 19 small passes and 12 "
     "resolves per frame for a flatter image.", kOnOff, false},
};
constexpr Option kGame[] = {
    {"mgr_skip_intro_logos", "Skip intro logos",
     "Skips any remaining boot logos. Already-finished logos are unaffected.", kOnOff, false},
};
constexpr Option kControls[] = {
    {"mnk_mode", "Keyboard controls", "Enable keyboard and mouse controller emulation. Applies immediately.", kOnOff, false},
    {"mgr_prompt_style", "Button prompts",
     "Automatic follows the keyboard or controller you used last.", kPrompts, false},
    {"mnk_mouse", "Mouse camera / Blade Mode",
     "Use the mouse as the right stick for camera movement and free-angle Blade Mode. "
     "Mouse capture follows window focus; arrow-key right-stick input still works.",
     kOnOff, false},
};

const char* ScaleLabel(const std::string& scale) {
  if (scale == "0") return "UNAVAILABLE";
  if (scale == "1.5") return "1920X1080";
  if (scale == "2") return "2560X1440";
  if (scale == "3") return "3840X2160";
  return "1280X720";
}

bool IsPendingRestart(const char* cvar, const std::string& requested) {
  if (std::string_view(cvar) == "mgr_render_target_precision")
    return requested != MgrActiveRenderTargetPrecision();
  return false;
}

bool DrawOption(const Option& option) {
  bool changed = false;
  const std::string current = rex::cvar::GetFlagByName(option.cvar);
  const Choice* selected = &option.choices.front();
  for (const Choice& choice : option.choices) {
    if (current == choice.value) selected = &choice;
  }
  if (game_style) {
    std::string shown = selected->label;
    if (std::string_view(option.cvar) == "mgr_resolution_scale") {
      const auto status = MgrGetResolutionStatus();
      const std::string state = current == status.requested ? status.state : "requested";
      shown += " - " + Uppercase(state) + " - ACTIVE: " + ScaleLabel(status.active);
      if (state == "failed" && !status.reason.empty()) shown += " - " + status.reason;
    } else if (std::string_view(option.cvar) == "mgr_render_target_precision") {
      const auto active = MgrActiveRenderTargetPrecision();
      const bool matches = current == active;
      shown = selected->label;
      shown += matches ? " - ACTIVE" :
          std::string(" - ACTIVE: ") + Uppercase(active) + " - RESTART TO APPLY";
    }
    const int step = MenuRow(option.cvar, option.label, shown.c_str(), option.help);
    if (!step) return false;
    const auto index = selected - option.choices.data();
    const auto& next = option.choices[(index + step + option.choices.size()) % option.choices.size()];
    const bool applied = rex::cvar::SetFlagByName(option.cvar, next.value);
    if (!applied) menu_help = "This setting could not be applied.";
    return applied && current != next.value;
  }
  ImGui::PushID(option.cvar);
  const float row_width = ImGui::GetContentRegionAvail().x;
  const float row_x = ImGui::GetCursorPosX();
  ImGui::TextUnformatted(option.label);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.help);
  ImGui::SameLine(row_x + row_width * 0.48f);
  ImGui::SetNextItemWidth(row_width * 0.42f);
  if (ImGui::BeginCombo("##v", selected->label)) {
    for (const Choice& choice : option.choices) {
      if (ImGui::Selectable(choice.label, &choice == selected)) {
        rex::cvar::SetFlagByName(option.cvar, choice.value);
        changed = current != choice.value;
      }
    }
    ImGui::EndCombo();
  }
  if (std::string_view(option.cvar) == "mgr_resolution_scale") {
    const auto status = MgrGetResolutionStatus();
    const std::string state = current == status.requested ? status.state : "requested";
    ImGui::TextWrapped("%s - Active: %s", state.c_str(), ScaleLabel(status.active));
    if (state == "failed" && !status.reason.empty())
      ImGui::TextWrapped("%s", status.reason.c_str());
  }
  if (option.restart && IsPendingRestart(option.cvar, current)) {
    ImGui::SameLine();
    ImGui::TextDisabled("*");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies after restart");
  }
  ImGui::PopID();
  return changed;
}

bool DrawSection(const char* title, std::span<const Option> options) {
  bool changed = false;
  if (game_style) {
    ImGui::PushFont(MgrMenuLabelFont(), 27 * menu_scale);
    ImGui::TextColored(ImVec4(0.33f, 0.77f, 0.83f, 1), "%s", title);
    ImGui::PopFont();
  } else ImGui::SeparatorText(title);
  for (const Option& option : options) changed |= DrawOption(option);
  return changed;
}
}  // namespace

MgrOptionsDialog::MgrOptionsDialog(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input,
                                   std::filesystem::path config_path, rex::ui::Window* window,
                                   bool integrated)
    : ImGuiDialog(drawer), input_(input), window_(window), config_path_(std::move(config_path)),
      integrated_(integrated) {
  if (window_) {
    output_width_ = int(window_->GetActualPhysicalWidth());
    output_height_ = int(window_->GetActualPhysicalHeight());
  }
  if (integrated_) {
    if (window_) {
      window_->AddInputListener(this, 65);  // before ImGui's event collector (64)
      window_->AddListener(this);
    }
    SDL_AddEventWatch(WatchController, this);
  } else {
    SetInputBlocked(true);
  }
}

bool MgrOptionsDialog::DrawOutputResolution() {
  const std::string current = rex::cvar::GetFlagByName("mgr_output_resolution");
  if (last_output_selection_ != current) {
    last_output_selection_ = current;
    custom_output_ = current != "source" && current != "display" && current != "1920x1080";
    if (custom_output_) {
      int32_t width = 0, height = 0;
      if (rex::graphics::video_mode_util::TryParseResolutionPreset(current, width, height)) {
        output_width_ = width;
        output_height_ = height;
      }
    }
  }
  bool changed = false;
  if (game_style) {
    std::string selected = custom_output_ ? "CUSTOM" : current == "display" ?
        "MATCH DISPLAY (" + Uppercase(rex::cvar::GetFlagByName("vulkan_output_resolution")) + ")" :
        current == "1920x1080" ? "1920X1080 OUTPUT" : "SOURCE IMAGE";
    const int step = MenuRow("output", "Output image size", selected.c_str(),
        "LIVE OUTPUT IMAGE SIZE: resamples the completed game image. It does not change internal "
        "3D rendering detail. Source avoids the extra resize pass.");
    if (step) {
      int mode = custom_output_ ? 3 : current == "1920x1080" ? 2 : current == "display" ? 1 : 0;
      mode = (mode + step + 4) % 4;
      custom_output_ = mode == 3;
      if (!custom_output_) {
        const char* value = mode == 1 ? "display" : mode == 2 ? "1920x1080" : "source";
        changed = current != value && rex::cvar::SetFlagByName("mgr_output_resolution", value);
      }
    }
  } else {
  const float row_width = ImGui::GetContentRegionAvail().x;
  const float row_x = ImGui::GetCursorPosX();
  ImGui::TextUnformatted("Output image size (live)");
  ImGui::SameLine(row_x + row_width * 0.48f);
  ImGui::SetNextItemWidth(row_width * 0.42f);
  const char* preview = custom_output_ ? "Custom" : current == "display" ? "Match display"
                                                                          : "Source image";
  if (ImGui::BeginCombo("##output_resolution", preview)) {
    for (const Choice& choice : {Choice{"Source image", "source"},
                                 Choice{"Match display", "display"},
                                 Choice{"1920x1080 output", "1920x1080"}}) {
      if (ImGui::Selectable(choice.label, !custom_output_ && current == choice.value)) {
        rex::cvar::SetFlagByName("mgr_output_resolution", choice.value);
        custom_output_ = false;
        changed |= current != choice.value;
      }
    }
    if (ImGui::Selectable("Custom", custom_output_)) custom_output_ = true;
    ImGui::EndCombo();
  }
  }
  if (game_style) {
    const std::string applied = rex::cvar::GetFlagByName("vulkan_output_resolution");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3 * menu_scale);
    ImGui::PushFont(MgrMenuValueFont(), 18 * menu_scale);
    ImGui::TextColored(ImVec4(0.58f, 0.76f, 0.78f, 1),
        "LIVE OUTPUT TARGET: %s  |  INTERNAL RENDER DETAIL IS CONTROLLED BELOW",
        Uppercase(applied).c_str());
    ImGui::PopFont();
  }
  if (custom_output_) {
    ImGui::SetNextItemWidth(game_style ? 145 * menu_scale : 120.0f);
    ImGui::InputInt("Width", &output_width_, 1, 64);
    bool edit_finished = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(game_style ? 145 * menu_scale : 120.0f);
    ImGui::InputInt("Height", &output_height_, 1, 64);
    edit_finished |= ImGui::IsItemDeactivatedAfterEdit();
    const bool valid = output_width_ >= 64 && output_width_ <= 8192 &&
                       output_height_ >= 64 && output_height_ <= 8192;
    ImGui::BeginDisabled(!valid);
    const bool apply = ImGui::Button("Apply output size");
    if (valid && (apply || edit_finished)) {
      const std::string value = std::to_string(output_width_) + "x" + std::to_string(output_height_);
      changed |= current != value && rex::cvar::SetFlagByName("mgr_output_resolution", value);
    }
    ImGui::EndDisabled();
    if (!valid) ImGui::TextDisabled("Each dimension must be 64..8192 pixels.");
  }
  if (game_style) return changed;
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
  if (integrated_) {
    rex::cvar::SetFlagByName("imgui_guest_paced", "false");
    SDL_RemoveEventWatch(WatchController, this);
    if (window_) {
      window_->RemoveInputListener(this);
      window_->RemoveListener(this);
    }
    ReleaseController(GetIO());
  }
  SavePending();
  SetInputBlocked(false);
}

void MgrOptionsDialog::OnDraw(ImGuiIO& io) {
  if (integrated_) {
    DrawIntegrated(io);
    return;
  }
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
  if (changed) { dirty_ = true; saved_ = false; }
  ImGui::TextWrapped("Free Blade Mode uses Blade Mode + right-stick direction. "
                     "Keyboard defaults: hold Q and use the arrow keys, or enable mouse input.");
  ImGui::TextDisabled("Rebind keys: OPTIONS > KEYBOARD / MOUSE.");

  ImGui::Separator();
  if (ImGui::Button("Save")) {
    dirty_ = true;
    SavePending();
  }
  ImGui::SameLine();
  ImGui::TextDisabled(saved_ ? "Saved to %s" : "Saved to %s when closed",
                      config_path_.filename().string().c_str());
  ImGui::End();
}

namespace {
struct ActionBinding {
  const char* cvar;
  const char* label;
  const char* group;
};
constexpr ActionBinding kBindings[] = {
    {"keybind_lstick_up", "Move forward", "MOVEMENT"},
    {"keybind_lstick_down", "Move backward", nullptr},
    {"keybind_lstick_left", "Move left", nullptr},
    {"keybind_lstick_right", "Move right", nullptr},
    {"keybind_lstick_press", "Left stick press", nullptr},
    {"keybind_x", "Light attack", "COMBAT / ACTIONS"},
    {"keybind_y", "Heavy attack", nullptr},
    {"keybind_a", "Jump / confirm", nullptr},
    {"keybind_b", "Action / cancel", nullptr},
    {"keybind_left_trigger", "Blade Mode", nullptr},
    {"keybind_right_trigger", "Ninja Run", nullptr},
    {"keybind_left_shoulder", "Sub-weapon", nullptr},
    {"keybind_right_shoulder", "Lock-on", nullptr},
    {"keybind_rstick_up", "Look / cut up", "CAMERA / FREE BLADE MODE"},
    {"keybind_rstick_down", "Look / cut down", nullptr},
    {"keybind_rstick_left", "Look / cut left", nullptr},
    {"keybind_rstick_right", "Look / cut right", nullptr},
    {"keybind_rstick_press", "Right stick press", nullptr},
    {"keybind_dpad_up", "Equipment up", "MENUS / EQUIPMENT"},
    {"keybind_dpad_down", "Equipment down", nullptr},
    {"keybind_dpad_left", "Equipment left", nullptr},
    {"keybind_dpad_right", "Equipment right", nullptr},
    {"keybind_start", "Pause / menu", nullptr},
    {"keybind_back", "Codec / back", nullptr},
};

std::string_view TrimBinding(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::vector<std::string> BindingTokens(std::string_view s) {
  std::vector<std::string> result;
  while (!s.empty()) {
    const size_t end = std::min(s.find(','), s.size());
    const auto token = TrimBinding(s.substr(0, end));
    if (!token.empty()) result.emplace_back(token);
    s.remove_prefix(std::min(end + 1, s.size()));
  }
  return result;
}

std::string BindingIdentity(std::string_view value) {
  unsigned modifiers = 0;
  for (;;) {
    const size_t plus = value.find('+');
    if (plus == std::string_view::npos) break;
    const auto prefix = TrimBinding(value.substr(0, plus));
    if (prefix == "Shift") modifiers |= 1;
    else if (prefix == "Control" || prefix == "Ctrl") modifiers |= 2;
    else if (prefix == "Alt") modifiers |= 4;
    else break;
    value.remove_prefix(plus + 1);
  }
  const auto key = rex::ui::ParseVirtualKey(TrimBinding(value));
  using K = rex::ui::VirtualKey;
  if (key == K::kShift) modifiers |= 1;
  if (key == K::kControl) modifiers |= 2;
  if (key == K::kMenu) modifiers |= 4;
  return std::to_string(modifiers) + ":" + std::to_string(uint16_t(key));
}

std::string DisplayBindings(std::string_view value) {
  std::string text;
  for (const auto& token : BindingTokens(value)) {
    if (!text.empty()) text += " / ";
    text += token;  // Full names in settings; compact symbols belong on HUD keycaps.
  }
  return text.empty() ? "Unbound" : text;
}

std::string ModifierName(rex::ui::VirtualKey key) {
  using K = rex::ui::VirtualKey;
  if (key == K::kShift || key == K::kLShift || key == K::kRShift) return "Shift";
  if (key == K::kControl || key == K::kLControl || key == K::kRControl) return "Control";
  if (key == K::kMenu || key == K::kLMenu || key == K::kRMenu) return "Alt";
  return {};
}

constexpr uint32_t PadBit(SDL_GamepadButton button) { return 1u << unsigned(button); }
constexpr std::array<std::pair<SDL_GamepadButton, ImGuiKey>, 6> kPadNavigation = {{
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
    {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
    {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
}};
}  // namespace

void MgrOptionsDialog::SetInputBlocked(bool blocked) {
  if (blocked == input_blocked_) return;
  if (integrated_) MgrGameOptionsSetPageInput(blocked);
  if (input_) {
    if (blocked) input_->AddUIInputBlocker();
    else input_->RemoveUIInputBlocker();
  }
  input_blocked_ = blocked;
}

void MgrOptionsDialog::SavePending() {
  if (!dirty_) return;
  MgrSaveConfig(config_path_);
  dirty_ = false;
  saved_ = true;
}

void MgrOptionsDialog::SelectPage(Page page) {
  if (page == page_) return;
  capturing_bind_.clear();
  pending_modifier_.clear();
  SavePending();
  page_ = page;
  focus_first_ = true;
  SetInputBlocked(page != Page::kSystem);
  if (page == Page::kSystem) ReleaseController(GetIO());
}

void MgrOptionsDialog::StartCapture(const char* name, bool append) {
  capturing_bind_ = name;
  pending_modifier_.clear();
  capture_append_ = append;
  capture_started_frame_ = ImGui::GetFrameCount();
}

void MgrOptionsDialog::FinishCapture(std::string key) {
  if (capturing_bind_.empty() || key.empty()) return;
  std::string value = key;
  if (capture_append_) {
    value = rex::cvar::GetFlagByName(capturing_bind_);
    const auto identity = BindingIdentity(key);
    for (const auto& token : BindingTokens(value)) {
      if (BindingIdentity(token) == identity) {
        capturing_bind_.clear();
        pending_modifier_.clear();
        return;
      }
    }
    if (!value.empty()) value += ',';
    value += key;
  }
  dirty_ |= rex::cvar::SetFlagByName(capturing_bind_, value);
  saved_ = false;
  capturing_bind_.clear();
  pending_modifier_.clear();
}

void MgrOptionsDialog::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!integrated_ || !native_visible_ || !MgrGameOptionsGetState().interactive) return;
  auto& io = GetIO();  // Select this drawer's context before querying ImGui.
  using K = rex::ui::VirtualKey;
  if (!capturing_bind_.empty()) {
    e.set_handled(true);
    if (e.prev_state() || ImGui::GetFrameCount() <= capture_started_frame_) return;
    if (e.virtual_key() == K::kEscape) {
      capturing_bind_.clear();
      pending_modifier_.clear();
      return;
    }
    if (const std::string modifier = ModifierName(e.virtual_key()); !modifier.empty()) {
      pending_modifier_ = modifier;
      return;
    }
    std::string key = rex::ui::VirtualKeyToString(e.virtual_key());
    if (key.empty() || e.is_super_pressed()) return;
    std::string chord;
    if (e.is_ctrl_pressed()) chord += "Control+";
    if (e.is_shift_pressed()) chord += "Shift+";
    if (e.is_alt_pressed()) chord += "Alt+";
    FinishCapture(chord + key);
    return;
  }
  if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
  if ((e.virtual_key() == K::kPrior || e.virtual_key() == K::kNext) && !e.prev_state()) {
    const int direction = e.virtual_key() == K::kNext ? 1 : 2;
    SelectPage(Page((int(page_) + direction) % 3));
    e.set_handled(true);
  } else if (page_ != Page::kSystem && e.virtual_key() == K::kEscape) {
    SelectPage(Page::kSystem);
    e.set_handled(true);
  }
}

void MgrOptionsDialog::OnKeyUp(rex::ui::KeyEvent& e) {
  if (!capturing_bind_.empty() && !pending_modifier_.empty() &&
      ModifierName(e.virtual_key()) == pending_modifier_) {
    FinishCapture(pending_modifier_);
  }
  // Always propagate releases to MnK. Consuming a key-up can leave a key that
  // was down before capture stuck in the guest input driver.
}

void MgrOptionsDialog::OnMouseDown(rex::ui::MouseEvent& e) {
  if (capturing_bind_.empty()) return;
  // Consume before ImGui sees this press: assigning LMB over Reset/Clear must
  // not also activate that widget. Releases still propagate, as for keys.
  e.set_handled(true);
  GetIO();
  if (ImGui::GetFrameCount() <= capture_started_frame_) return;
  using B = rex::ui::MouseEvent::Button;
  const char* key = nullptr;
  switch (e.button()) {
    case B::kLeft: key = "LMB"; break;
    case B::kRight: key = "RMB"; break;
    case B::kMiddle: key = "MMB"; break;
    case B::kX1: key = "X1MB"; break;
    case B::kX2: key = "X2MB"; break;
    default: return;
  }
  const auto mods = SDL_GetModState();
  if (mods & SDL_KMOD_GUI) return;
  std::string chord;
  if (mods & SDL_KMOD_CTRL) chord += "Control+";
  if (mods & SDL_KMOD_SHIFT) chord += "Shift+";
  if (mods & SDL_KMOD_ALT) chord += "Alt+";
  FinishCapture(chord + key);
}

void MgrOptionsDialog::OnGotFocus(rex::ui::UISetupEvent&) { has_focus_ = true; }

void MgrOptionsDialog::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  capturing_bind_.clear();
  pending_modifier_.clear();
  controller_buttons_.store(0);
  controller_x_.store(0);
  controller_y_.store(0);
}

bool MgrOptionsDialog::WatchController(void* userdata, SDL_Event* event) {
  auto& self = *static_cast<MgrOptionsDialog*>(userdata);
  SDL_JoystickID id = 0;
  bool deliberate = false;
  if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
    id = event->gbutton.which;
    deliberate = event->gbutton.down;
  } else if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    if (event->gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && event->gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY)
      return true;
    id = event->gaxis.which;
    deliberate = std::abs(int(event->gaxis.value)) > 16000;
  } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
    if (self.controller_.load() == event->gdevice.which) {
      self.controller_.store(0);
      self.controller_buttons_.store(0);
      self.controller_x_.store(0);
      self.controller_y_.store(0);
    }
    return true;
  } else return true;
  if (self.controller_.load() != id) {
    if (!deliberate) return true;
    self.controller_.store(id);
    self.controller_buttons_.store(0);
    self.controller_x_.store(0);
    self.controller_y_.store(0);
  }
  if (event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? self.controller_x_ : self.controller_y_)
        .store(event->gaxis.value);
  } else if (event->gbutton.button < 32) {
    const uint32_t bit = 1u << event->gbutton.button;
    if (event->gbutton.down) self.controller_buttons_.fetch_or(bit);
    else self.controller_buttons_.fetch_and(~bit);
  }
  return true;
}

void MgrOptionsDialog::ReleaseController(ImGuiIO& io) {
  for (const auto& [button, key] : kPadNavigation) {
    if (injected_controller_ & PadBit(button)) io.AddKeyEvent(key, false);
  }
  injected_controller_ = 0;
  if (nav_owned_) {
    constexpr int flags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags = (io.ConfigFlags & ~flags) | (saved_nav_flags_ & flags);
    if (!saved_backend_gamepad_) io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    nav_owned_ = false;
  }
}

void MgrOptionsDialog::PollController(ImGuiIO& io) {
  if (!has_focus_) {
    previous_controller_ = controller_buttons_.load();
    ReleaseController(io);
    return;
  }
  uint32_t buttons = controller_buttons_.load();
  const int x = controller_x_.load(), y = controller_y_.load();
  if (x < -16000) buttons |= PadBit(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
  if (x > 16000) buttons |= PadBit(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  if (y < -16000) buttons |= PadBit(SDL_GAMEPAD_BUTTON_DPAD_UP);
  if (y > 16000) buttons |= PadBit(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
  const uint32_t pressed = buttons & ~previous_controller_;
  previous_controller_ = buttons;
  const bool capturing = !capturing_bind_.empty();
  if (capturing) {
    if (pressed & PadBit(SDL_GAMEPAD_BUTTON_EAST)) {
      capturing_bind_.clear();
      pending_modifier_.clear();
    }
  } else if (!io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
    if (pressed & PadBit(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
      SelectPage(Page((int(page_) + 2) % 3));
    if (pressed & PadBit(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))
      SelectPage(Page((int(page_) + 1) % 3));
    if (page_ != Page::kSystem && (pressed & PadBit(SDL_GAMEPAD_BUTTON_EAST)))
      SelectPage(Page::kSystem);
  }
  if (page_ == Page::kSystem) return;
  if (!nav_owned_) {
    saved_nav_flags_ = io.ConfigFlags;
    saved_backend_gamepad_ = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
    nav_owned_ = true;
  }
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  if (controller_.load()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  for (const auto& [button, key] : kPadNavigation) {
    const uint32_t bit = PadBit(button);
    const bool down = !capturing && (buttons & bit) != 0;
    if (down != ((injected_controller_ & bit) != 0)) io.AddKeyEvent(key, down);
  }
  injected_controller_ = capturing ? 0 : buttons;
}

bool MgrOptionsDialog::DrawKeyboard() {
  bool changed = DrawSection("KEYBOARD / MOUSE", kControls);
  float sensitivity = std::strtof(rex::cvar::GetFlagByName("mnk_sensitivity").c_str(), nullptr);
  if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.01f, 10.0f, "%.2f"))
    changed |= rex::cvar::SetFlagByName("mnk_sensitivity", std::to_string(sensitivity));
  ImGui::TextWrapped("Hold Blade Mode and use the camera direction keys for free-angle cutting. "
                     "Action names below follow controller Type A; the System page retains "
                     "the game's controller layout selector.");
  ImGui::TextDisabled("Select a binding to replace it, or Add for an alternative. Esc cancels capture.");
  if (!capturing_bind_.empty()) {
    ImGui::TextColored(ImVec4(1, 0.85f, 0.3f, 1), "PRESS A KEY OR MOUSE BUTTON...");
  }
  // Read each live cvar once per draw. Count distinct action owners per chord;
  // aliases repeated within one action aren't a conflict with another action.
  std::array<std::string, std::size(kBindings)> values;
  std::array<std::unordered_set<std::string>, std::size(kBindings)> identities;
  std::unordered_map<std::string, unsigned> owners;
  for (size_t i = 0; i < std::size(kBindings); ++i) {
    values[i] = rex::cvar::GetFlagByName(kBindings[i].cvar);
    for (const auto& token : BindingTokens(values[i])) identities[i].insert(BindingIdentity(token));
    for (const auto& identity : identities[i]) ++owners[identity];
  }
  ImGui::BeginDisabled(!capturing_bind_.empty());
  for (size_t i = 0; i < std::size(kBindings); ++i) {
    const auto& binding = kBindings[i];
    if (binding.group) {
      ImGui::PushFont(MgrMenuLabelFont(), 27 * menu_scale);
      ImGui::TextColored(ImVec4(0.33f, 0.77f, 0.83f, 1), "%s", binding.group);
      ImGui::PopFont();
    }
    const std::string& value = values[i];
    ImGui::PushID(binding.cvar);
    ImGui::PushFont(MgrMenuLabelFont(), 30 * menu_scale);
    ImGui::TextUnformatted(Uppercase(binding.label).c_str());
    ImGui::PopFont();
    const float row_width = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMin().x + row_width * 0.42f);
    const bool capturing = capturing_bind_ == binding.cvar;
    const std::string label = capturing ? "Press key..." : DisplayBindings(value);
    if (ImGui::Button(label.c_str(), ImVec2(row_width * 0.27f, 0)))
      StartCapture(binding.cvar, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add")) StartCapture(binding.cvar, true);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
      if (const auto* info = rex::cvar::GetFlagInfo(binding.cvar))
        changed |= rex::cvar::SetFlagByName(binding.cvar, info->default_value);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) changed |= rex::cvar::SetFlagByName(binding.cvar, "");
    // Exact chord matching keeps Shift+arrows distinct from bare arrows.
    bool conflict = false;
    for (const auto& identity : identities[i]) conflict |= owners.at(identity) > 1;
    if (conflict) {
      ImGui::TextColored(ImVec4(1, 0.76f, 0.2f, 1), "Shared with another action");
    }
    ImGui::PopID();
  }
  ImGui::EndDisabled();
  return changed;
}

void MgrOptionsDialog::DrawIntegrated(ImGuiIO& io) {
  const auto state = MgrGameOptionsGetState();
  if (!state.interactive) {
    if (native_visible_) {
      rex::cvar::SetFlagByName("imgui_guest_paced", "false");
      SavePending();
      capturing_bind_.clear();
      pending_modifier_.clear();
      page_ = Page::kSystem;
      SetInputBlocked(false);
      ReleaseController(io);
    }
    native_visible_ = false;
    return;
  }
  if (!native_visible_ || session_ != state.session) {
    // cOptionMenu still renders guest frames while its input/update is parked.
    // Follow those frames instead of presenting the same menu in a tight loop.
    rex::cvar::SetFlagByName("imgui_guest_paced", "true");
    SelectPage(Page::kSystem);
    session_ = state.session;
    previous_controller_ = controller_buttons_.load();
    native_visible_ = true;
  }
  PollController(io);
  const float scale = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f);
  const ImVec2 origin((io.DisplaySize.x - 1280 * scale) * 0.5f,
                      (io.DisplaySize.y - 720 * scale) * 0.5f);
  game_style = true;
  menu_scale = scale;
  if (menu_help.empty()) menu_help = "Select a setting. Left / right changes its value. Live settings apply immediately; restart settings are marked.";
  ImGui::PushFont(MgrMenuValueFont(), 23 * scale);
  if (page_ != Page::kSystem) {
    // The guest System/Audio widgets continue rendering behind this host page.
    // Mask them while the replacement child page owns the menu content.
    auto* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(2, 10, 15, 255));
    bg->AddRectFilledMultiColor(ImVec2(0, 0), io.DisplaySize,
        IM_COL32(3, 18, 24, 255), IM_COL32(2, 7, 12, 255),
        IM_COL32(5, 25, 32, 255), IM_COL32(2, 7, 12, 255));
    for (float y = 0; y < io.DisplaySize.y; y += 4 * scale)
      bg->AddLine(ImVec2(0, y), ImVec2(io.DisplaySize.x, y), IM_COL32(80, 190, 210, 9));
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.88f, 0.9f, 1));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.025f, 0.09f, 0.12f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.29f, 0.37f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.4f, 0.5f, 1));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.025f, 0.09f, 0.12f, 1));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.3f, 0.38f, 1));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.12f, 0.4f, 0.5f, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.01f, 0.03f, 0.045f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.2f, 0.5f, 0.57f, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.18f, 0.45f, 0.5f, 0.6f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.3f, 0.7f, 0.8f, 1));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(0.4f, 0.85f, 0.95f, 1));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.09f, 0.27f, 0.34f, 1));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.12f, 0.35f, 0.43f, 1));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.3f, 0.7f, 0.8f, 1));
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.4f, 0.85f, 0.95f, 1));
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.35f, 0.85f, 0.94f, 1));
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.47f, 0.64f, 0.67f, 1));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.01f, 0.05f, 0.07f, 1));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.12f, 0.4f, 0.5f, 1));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.35f, 0.85f, 0.94f, 1));
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.12f, 0.4f, 0.5f, 0.7f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9 * scale, 6 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * scale, 4 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 3 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4 * scale);
  constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
  if (page_ != Page::kSystem) {
    // The retail System/Audio screen already owns this region. Keep every
    // host-drawn navigation control off that page; PgUp/PgDn and shoulders
    // remain available to enter these supplemental pages.
    ImGui::SetNextWindowPos(ImVec2(origin.x + 95 * scale, origin.y + 26 * scale));
    ImGui::SetNextWindowSize(ImVec2(1090 * scale, 54 * scale));
    ImGui::Begin("##MGRGameOptionsPages", nullptr, flags | ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(MgrMenuLabelFont(), 29 * scale);
    constexpr const char* names[] = {"SYSTEM / AUDIO", "GRAPHICS", "KEYBOARD / MOUSE"};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine();
      const bool selected = int(page_) == i;
      if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.35f, 0.44f, 1));
      if (ImGui::Button(names[i], ImVec2(350 * scale, 42 * scale))) SelectPage(Page(i));
      if (selected) ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(origin.x + 95 * scale, origin.y + 92 * scale));
    ImGui::SetNextWindowSize(ImVec2(1090 * scale, 520 * scale));
    ImGui::Begin("##MGRGameOptionsBody", nullptr, flags | ImGuiWindowFlags_NoBackground);
    ImGui::BeginChild("##rows", ImVec2(0, 0), false);
    if (focus_first_) {
      ImGui::SetWindowFocus();
      ImGui::SetKeyboardFocusHere();
      focus_first_ = false;
    }
    bool changed = false;
    if (page_ == Page::kGraphics) {
      changed = DrawSection("DISPLAY", kDisplay);
      changed |= DrawOutputResolution();
      changed |= DrawSection("GRAPHICS", kGraphics);
      changed |= DrawSection("GAME", kGame);
    } else changed = DrawKeyboard();
    if (changed) { dirty_ = true; saved_ = false; }
    ImGui::EndChild();
    ImGui::End();
    if (page_ == Page::kGraphics) {
      ImGui::SetNextWindowPos(ImVec2(origin.x + 770 * scale, origin.y + 460 * scale));
      ImGui::SetNextWindowSize(ImVec2(410 * scale, 155 * scale));
      ImGui::Begin("##MGRSettingHelp", nullptr, flags | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
      ImGui::TextWrapped("%s", menu_help.c_str());
      ImGui::End();
    }
    ImGui::SetNextWindowPos(ImVec2(origin.x + 95 * scale, origin.y + 630 * scale));
    ImGui::SetNextWindowSize(ImVec2(1090 * scale, 65 * scale));
    ImGui::Begin("##MGRGameOptionsFooter", nullptr, flags | ImGuiWindowFlags_NoBackground);
    if (ImGui::Button("SAVE")) { dirty_ = true; SavePending(); }
    ImGui::SameLine();
    if (ImGui::Button("BACK TO SYSTEM")) SelectPage(Page::kSystem);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dirty_ ? "Unsaved changes" : saved_ ? "Saved" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("GAME: %.1f FPS", MgrGuestFPS());
    ImGui::TextDisabled("Left / Right: change   PgUp / PgDn: page   Esc: back");
    ImGui::End();
  }
  ImGui::PopStyleVar(8);
  ImGui::PopStyleColor(24);
  ImGui::PopFont();
  game_style = false;
}
