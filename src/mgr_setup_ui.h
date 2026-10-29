// Drawing kit for the first-run setup screen: fonts, palette, the key-art
// backdrop and a cut-corner panel that lays out its content top to bottom.
#pragma once

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace mgr_setup_ui {

// Palette (matches the HTML prototype).
inline constexpr ImU32 kVoid = IM_COL32(11, 16, 20, 255);
inline constexpr ImU32 kStorm = IM_COL32(27, 38, 45, 255);
inline constexpr ImU32 kBone = IM_COL32(235, 232, 227, 255);
inline constexpr ImU32 kBody = IM_COL32(207, 203, 197, 255);
inline constexpr ImU32 kAsh = IM_COL32(138, 151, 159, 255);
inline constexpr ImU32 kRed = IM_COL32(196, 20, 29, 255);
inline constexpr ImU32 kRedHot = IM_COL32(255, 42, 53, 255);
inline constexpr ImU32 kIce = IM_COL32(134, 230, 242, 255);
inline constexpr ImU32 kPanel = IM_COL32(8, 13, 17, 205);
inline constexpr ImU32 kLine = IM_COL32(235, 232, 227, 41);

struct Fonts {
  ImFont* big = nullptr;      // display, install percentage
  ImFont* heading = nullptr;  // display, screen heading
  ImFont* mark = nullptr;     // display, MGR.R. wordmark
  ImFont* body = nullptr;
  ImFont* option = nullptr;   // body medium, option rows
  ImFont* small = nullptr;    // body, hints and prompts
  ImFont* mono = nullptr;
  ImFont* mono_small = nullptr;
};
const Fonts& GetFonts();
// Adds the setup faces to the atlas (from Resources/setup/fonts or the build
// dir). Missing files fall back to ImGui's default font.
void LoadFonts(ImFontAtlas* atlas);

// setup/ beside the executable (dev builds) or in Contents/Resources (.app).
std::filesystem::path AssetDir();

// The screen is laid out in design units (kDesignHeight tall) and scaled up
// to the display afterwards; this converts the mouse back for hit tests.
inline constexpr float kDesignHeight = 900.0f;
void SetScale(float scale);
bool MouseIn(ImVec2 a, ImVec2 b);

// Key art behind the panel. Created lazily: textures can only be made while
// the drawer has a frame open.
class Backdrop {
 public:
  void Draw(ImDrawList* dl, ImVec2 size, rex::ui::ImmediateDrawer* drawer);

 private:
  bool tried_ = false;
  int width_ = 0;
  int height_ = 0;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
};

// Text helpers. Spaced text adds letter spacing (uppercase labels).
void Text(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, std::string_view text);
// size 0 draws at the font's loaded size.
float SpacedWidth(ImFont* font, std::string_view text, float spacing, float size = 0);
void SpacedText(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, std::string_view text,
                float spacing, float size = 0);

// One row of a key/value list.
struct Fact {
  std::string label;
  std::string value;
  bool mono = false;
};

// A check-list row: state 0 pending, 1 passed, 2 failed.
struct CheckRow {
  std::string label;
  std::string value;
  int state = 0;
};

// Lays out a screen's content inside the left-hand panel. The content function
// runs twice per frame: once to measure, once to draw, so it must not change
// state itself; it reports activations through Activated().
class Panel {
 public:
  using Content = std::function<void(Panel&)>;

  // Draws the panel under the wordmark and header ("INSTALLER") and returns
  // the id of the option activated this frame (empty if none).
  std::string Draw(ImDrawList* dl, ImVec2 screen, std::string_view header, const Content& content,
                   int& focus, bool confirm, bool mouse_enabled);

  void Eyebrow(std::string_view text);
  void Heading(std::string_view text);
  void Paragraph(std::string_view text, ImU32 color = kBody, ImFont* font = nullptr);
  void Facts(const std::vector<Fact>& facts);
  void Checks(const std::vector<CheckRow>& rows);
  void Alert(std::string_view text, ImU32 accent = kRedHot);
  void Progress(float fraction, std::string_view big, std::string_view caption);
  void Stats(const std::vector<Fact>& stats);
  void Log(const std::vector<std::pair<std::string, bool>>& lines);  // (text, current)
  void Option(std::string id, std::string_view label, std::string_view hint = {},
              bool quiet = false);
  void Gap(float height) { y_ += height; }

  float width() const { return width_; }
  int option_count() const { return option_count_; }
  // Bottom-right of the panel's footer row, for Prompts().
  ImVec2 FooterAnchor() const { return footer_anchor_; }

 private:
  void Begin(ImDrawList* dl, float x, float y, float width, bool measure);

  ImDrawList* dl_ = nullptr;
  bool measure_ = true;
  float x_ = 0, y_ = 0, width_ = 0;
  int option_count_ = 0;
  int* focus_ = nullptr;
  bool confirm_ = false;
  bool mouse_enabled_ = true;
  std::string activated_;
  ImVec2 footer_anchor_{0, 0};
};

// Bottom-row input prompts; pad switches to controller glyphs.
void Prompts(ImDrawList* dl, ImVec2 right_baseline, bool pad, bool with_back);

}  // namespace mgr_setup_ui
