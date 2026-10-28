// Native Xbox 360 Options extension, executable 4B4E080A / 0.0.0.4.
// Reuses the retail UID instances, MCD renderer, row animation states and audio.
// No host UI rendering or host input polling is used by this menu.
#include "mgr_native_options.h"
#include "mgr_settings.h"
#include "generated/default/mgr_pch.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

REX_EXTERN(sub_82936CF0);  // Ui::playAnimation(ui, node, state)
REX_EXTERN(sub_82C40DD8);  // OptionGroup::selectRow(group, absolute row, selected)
REX_EXTERN(sub_82C40F08);  // OptionGroup::setActive(group, active)
REX_EXTERN(sub_82886A90);  // native menu sound, shared-string argument
REX_EXTERN(sub_82C46620);  // existing description animation/update
REX_EXTERN(sub_82C4C6A0);  // existing help text for a retail row
REX_EXTERN(sub_8293F530);  // Ui::setNodeY(owner, node, f1 = y), rounded like the retail cursor

namespace {
constexpr uint32_t kInput = 0x83739CD0; // verified lis/addi in 82C71590
constexpr uint32_t kGroupOffsets[] = {28, 296, 564};
constexpr uint32_t kTextVtable = 0x821CBF50;
// Selection cursor (traced from the retail update, 82C718B0..82C71968): its node
// at +860 animates toward the float target at +956 while +964 is set. Targets
// are row * 44, plus 6 past each group header (rows 6 and 9).
constexpr uint32_t kCursorNode = 860, kCursorTarget = 956, kCursorMoving = 964;
// Group containers (one per OptionGroup) and the node Y that 8293F530 writes
// (node+672+196, a rounded float).
constexpr uint32_t kGroupContainers = 848, kNodeY = 868;
constexpr float kCursorRowStep = 44.0f, kCursorGroupGap = 6.0f;
constexpr int kDisplayFirstRow = 6, kSoundFirstRow = 9;
// OptionGroup node ids for the two System slider bars (built by 82C44810).
constexpr uint32_t kSliderSegmentIds = 136, kSliderSegments = 20;
enum class Page { Categories, Retail, Video, Misc };
// The first three categories are the retail System/Display/Audio groups.
constexpr const char* kCategories[] = {"SYSTEM", "DISPLAY", "AUDIO", "VIDEO", "MISC"};
constexpr int kFirstCustomCategory = 3;
constexpr Page kCustomPages[] = {Page::Video, Page::Misc};
// Retail row slots per page; longer pages scroll through them.
constexpr int kVisibleRows = 6;

uint8_t* Guest(uint8_t* base, uint32_t a) { return REX_RAW_ADDR(a); }
const uint8_t* Guest(const uint8_t* base, uint32_t a) { return REX_RAW_ADDR(a); }
uint32_t U32(const uint8_t* base, uint32_t a) {
  uint32_t v; std::memcpy(&v, Guest(base, a), 4); return __builtin_bswap32(v);
}
uint16_t U16(const uint8_t* base, uint32_t a) {
  uint16_t v; std::memcpy(&v, Guest(base, a), 2); return __builtin_bswap16(v);
}
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v); std::memcpy(Guest(base, a), &v, 4);
}
void W16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v); std::memcpy(Guest(base, a), &v, 2);
}

uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc* fn,
              uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
  PPCContext call = ctx;
  call.r3.u64 = a; call.r4.u64 = b; call.r5.u64 = c;
  fn(call, base);
  return call.r3.u32;
}

struct Choice { const char* label; const char* value; };
struct Option { const char* key; const char* label; const char* help; std::span<const Choice> choices; };
constexpr Choice kScale[] = {{"1280X720", "1"}, {"1920X1080", "1.5"}, {"2560X1440", "2"}, {"3840X2160", "3"}};
constexpr Choice kBool[] = {{"ON", "true"}, {"OFF", "false"}};
constexpr Choice kRate[] = {{"60 FPS", "60"}, {"120 FPS (EXPERIMENTAL)", "120"}, {"UNLOCKED (EXPERIMENTAL)", "unlocked"}};
constexpr Choice kPacing[] = {{"LOW LATENCY", "true"}, {"COMPATIBILITY", "false"}};
constexpr Choice kFilter[] = {{"GAME DEFAULT", "game"}, {"OFF", "off"}, {"2X", "2x"}, {"4X", "4x"}, {"8X", "8x"}, {"16X", "16x"}};
constexpr Choice kPrecision[] = {{"HIGH (ACCURATE)", "high"}, {"FAST (RESTART REQUIRED)", "fast"}};
constexpr Choice kOutput[] = {{"MATCH GAME", "source"}, {"MATCH DISPLAY", "display"}, {"1920X1080 OUTPUT", "1920x1080"}, {"2560X1440 OUTPUT", "2560x1440"}};
constexpr Choice kLod[] = {{"GAME DEFAULT", "game"}, {"HIGH", "high"}, {"MAXIMUM", "max"}};
constexpr Choice kMsaa[] = {{"OFF", "0"}, {"2X", "2"}, {"4X", "4"}, {"8X", "8"}};
constexpr Choice kShadow[] = {{"ORIGINAL (1024)", "0"}, {"2048", "2048"}, {"4096", "4096"}};
constexpr Choice kBrightness[] = {{"0.7", "0.7"}, {"0.8", "0.8"}, {"0.9", "0.9"}, {"1.0 (DEFAULT)", "1"},
                                  {"1.1", "1.1"}, {"1.2", "1.2"}, {"1.3", "1.3"}};
const Option kInternalResolution = {"mgr_resolution_scale", "INTERNAL RESOLUTION", "Changes internal rendering detail. Applies after the GPU rebuilds its resources.", kScale};
// Every graphics, video and screen option, in one scrolling page.
const Option kVideo[] = {
  {"fullscreen", "FULLSCREEN", "Uses the current display in fullscreen mode.", kBool},
  {"mgr_output_resolution", "OUTPUT RESOLUTION", "Resizes the completed image. Internal rendering detail is controlled separately.", kOutput},
  kInternalResolution,
  {"mgr_msaa", "ANTI-ALIASING", "Multisampling smooths jagged edges. Higher settings cost frame rate. Applies after a restart.", kMsaa},
  {"mgr_transparency_aa", "TRANSPARENCY AA", "Smooths the edges of hair, fences and foliage. Needs anti-aliasing.", kBool},
  {"mgr_shadow_resolution", "SHADOW RESOLUTION", "Sharper, more detailed shadows. Higher settings use more video memory. Applies after a restart.", kShadow},
  {"present_letterbox", "PRESERVE ASPECT RATIO", "Preserves the original image proportions on wider or taller displays.", kBool},
  {"mgr_vsync", "VSYNC", "Syncs frames to the display refresh, preventing tearing. Frame rates above the refresh rate need it off.", kBool},
  {"mgr_frame_rate", "FRAME RATE", "60 FPS is the original timing. Higher rates are experimental.", kRate},
  {"vulkan_nonblocking_guest_present", "FRAME PACING", "Low latency avoids blocking the game while waiting for a display image.", kPacing},
  {"mgr_brightness", "BRIGHTNESS", "Brightens or darkens the image. 1.0 is the original look.", kBrightness},
  {"mgr_texture_filtering", "TEXTURE FILTERING", "Anisotropic filtering improves textures viewed at an angle.", kFilter},
  {"mgr_lod", "MODEL DETAIL", "Detail and draw distance of far away characters and objects. Higher settings cost frame rate.", kLod},
  {"mgr_render_target_precision", "RENDERING QUALITY", "High preserves the original gamma precision. Changing this setting requires a restart.", kPrecision},
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

struct NativeText {
  uint32_t widget = 0, original_context = 0, original_index = 0, original_line = 0, original_alignment = 0;
  uint32_t allocation = 0, mcd = 0, paragraph = 0, line = 0, letters = 0;
  std::unordered_map<char, uint16_t> glyphs;
  uint16_t font = 3;
  std::string shown;
};
struct Menu {
  uint32_t object = 0;
  Page page = Page::Categories;
  // row indexes Options(); scroll is the option shown in the first slot.
  int row = 0, category = 0, scroll = 0;
  bool ready = false, dirty = false;
  uint32_t scratch = 0;
  // Internal resolution chosen while Options is open, applied when it closes:
  // the rebuild drops the paused game's GPU-only backdrop and takes seconds.
  std::string pending_scale;
  // Retail category view: the selected group's container is lifted into the
  // first slot (raw float bits of its original Y saved), and the cursor targets
  // are offset by the same amount.
  uint32_t lifted_node = 0, lifted_y = 0;
  float cursor_bias = 0;
  std::unordered_map<uint32_t, uint32_t> visibility;
  std::unordered_map<uint32_t, NativeText> text;
  std::filesystem::path config;
} menu;

uint32_t Node(uint8_t* base, uint32_t owner, uint32_t id) {
  const uint32_t ui = U32(base, owner + 24);
  if (!ui || id >= U32(base, ui + 124)) return 0;
  return U32(base, ui + 120) + id * 1024;
}
void Visible(uint8_t* base, uint32_t owner, uint32_t id, bool visible) {
  const uint32_t node = Node(base, owner, id);
  if (!node) return;
  menu.visibility.try_emplace(node, U32(base, node + 944));
  W32(base, node + 944, visible ? menu.visibility.at(node) : 0);
}
void Restore(uint8_t* base) {
  for (const auto& [node, value] : menu.visibility) W32(base, node + 944, value);
  if (menu.lifted_node) W32(base, menu.lifted_node + kNodeY, menu.lifted_y);
  menu.lifted_node = 0; menu.cursor_bias = 0;
  for (auto& [_, t] : menu.text) {
    if (U32(base, t.widget + 20) != t.allocation) continue;
    W32(base, t.widget + 20, t.original_context);
    W32(base, t.widget + 168, t.original_index);
    W32(base, t.widget + 172, t.original_line);
    W32(base, t.widget + 12, t.original_alignment);
    W32(base, t.widget + 184, 0);
    t.shown.clear();
  }
}

// A private MCD message uses the loaded game's own glyphs and texture bindings.
// The 360 renderer decodes glyph IDs directly (829880C0), not symbol indices.
// Original resources and message tables remain intact; context pointers are
// restored before the retail handlers or destructors run.
bool SetText(uint8_t* base, uint32_t owner, uint32_t id, const std::string& value, uint16_t font = 3) {
  const uint32_t node = Node(base, owner, id);
  if (!node) return false;
  const uint32_t widget = U32(base, node + 1008);
  if (!widget || U32(base, widget) != kTextVtable) return false;
  auto& t = menu.text[widget];
  if (!t.widget) {
    const uint32_t context = U32(base, widget + 20);
    // Controller legend labels use the resident core font context. Find that
    // context through a live retail widget rather than assuming a pool address.
    uint32_t core_context = 0;
    const uint32_t legend = Node(base, menu.object, 12);
    if (legend) {
      const uint32_t legend_widget = U32(base, legend + 1008);
      if (legend_widget && U32(base, legend_widget) == kTextVtable)
        core_context = U32(base, legend_widget + 20);
    }
    const uint32_t source = core_context ? U32(base, core_context + 4) : 0;
    if (!context || !source) { std::fprintf(stderr, "[mgr-native-text] no source widget=%08X context=%08X core=%08X source=%08X\n", widget, context, core_context, source); menu.text.erase(widget); return false; }
    const uint32_t size = U32(base, source + 32) + U32(base, source + 36) * 8;
    if (size < 40 || size > 0x40000) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        std::fprintf(stderr, "[mgr-native-text] bad size=%u core=%08X source=%08X header:", size, core_context, source);
        for (uint32_t off = 0; off < 40; off += 4) std::fprintf(stderr, " %08X", U32(base, source + off));
        std::fprintf(stderr, "\n");
      }
      menu.text.erase(widget); return false;
    }
    t.widget = widget; t.original_context = context;
    t.original_index = U32(base, widget + 168); t.original_line = U32(base, widget + 172); t.original_alignment = U32(base, widget + 12);
    t.font = font;
    t.allocation = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(size + 64 + U32(base, source + 20) * 40 + 16384);
    if (!t.allocation) { menu.text.erase(widget); return false; }
    std::memcpy(Guest(base, t.allocation), Guest(base, core_context), 64);
    t.mcd = t.allocation + 64;
    std::memcpy(Guest(base, t.mcd), Guest(base, source), size);
    W32(base, t.allocation + 4, t.mcd);
    // Retail labels use font 8 from the shared core bank, not the larger
    // font 6 used by title text. Bit 0x4000 selects that bank and its textures.
    const uint32_t shared = U32(base, 0x83933450);
    if (shared) {
      for (uint32_t i = 0; i < U32(base, shared + 12); ++i) {
        const uint32_t symbol = shared + U32(base, shared + 8) + i * 8;
        if (U16(base, symbol) == 8 && U16(base, symbol + 2) < 128)
          t.glyphs[char(U16(base, symbol + 2))] = uint16_t(U32(base, symbol + 4) | 0x4000);
      }
    }
    if (font != 8) {
      for (uint32_t i = 0; i < U32(base, source + 12); ++i) {
        const uint32_t symbol = source + U32(base, source + 8) + i * 8;
        if (U16(base, symbol) == font && U16(base, symbol + 2) < 128)
          t.glyphs[char(U16(base, symbol + 2))] = uint16_t(U32(base, symbol + 4));
      }
    }
    t.paragraph = (size + 15) & ~15u;
    t.line = t.paragraph + 20; t.letters = t.line + 24 * 4;
    const uint32_t message = t.mcd + U32(base, t.mcd);
    W32(base, message, t.paragraph); W32(base, message + 4, 1);
    W32(base, t.mcd + t.paragraph, t.line);
    W32(base, t.mcd + t.paragraph + 4, 1);
    W32(base, t.mcd + t.paragraph + 8, 0);
    W32(base, t.mcd + t.paragraph + 12, 0);
    W32(base, t.mcd + t.paragraph + 16, uint32_t(t.font) << 16);
    W32(base, t.mcd + t.line, t.letters);
    W32(base, t.mcd + t.line + 4, 0);
    W32(base, t.mcd + t.line + 16, 39);
    W32(base, t.mcd + t.line + 20, 0);
    std::fprintf(stderr, "[mgr-native-options] text widget=%08X core=%08X glyphs=%zu\n", widget, source, t.glyphs.size());
  }
  const bool value_row = owner != menu.object && font == 3;
  if (t.shown == value && U32(base, widget + 20) == t.allocation) return true;
  if (U32(base, widget + 20) != t.allocation) {
    t.original_context = U32(base, widget + 20);
    t.original_index = U32(base, widget + 168);
    t.original_line = U32(base, widget + 172); t.original_alignment = U32(base, widget + 12);
  }
  const std::string text = value.substr(0, 180);
  uint32_t p = t.mcd + t.letters;
  uint32_t line_index = 0, line_count = 0, line_start = p;
  const auto finish_line = [&] {
    const uint32_t line = t.mcd + t.line + line_index * 24;
    W16(base, p, 0x8000); p += 2;
    W32(base, line, line_start - t.mcd); W32(base, line + 4, 0);
    W32(base, line + 8, line_count); W32(base, line + 12, line_count + 1);
    W32(base, line + 16, 39); W32(base, line + 20, 0);
    line_start = p; line_count = 0; ++line_index;
  };
  for (char c : text) {
    if (c == '\n' && line_index < 3) { finish_line(); continue; }
    const bool space = c == ' ';
    const auto found = t.glyphs.find(c);
    W16(base, p, space || found == t.glyphs.end() ? 0x8001 : found->second);
    W16(base, p + 2, space || found == t.glyphs.end() ? t.font : 0);
    p += 4;
    ++line_count;
  }
  finish_line();
  W32(base, t.mcd + t.paragraph + 4, line_index);
  if (value_row) W32(base, widget + 12, 0);
  W32(base, widget + 20, t.allocation);
  W32(base, widget + 168, 0); W32(base, widget + 172, 0); W32(base, widget + 184, 0);
  t.shown = value;
  return true;
}

void Sound(PPCContext& ctx, uint8_t* base, uint32_t name) {
  if (!menu.scratch) menu.scratch = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(64);
  if (!menu.scratch) return;
  W32(base, menu.scratch, name); W32(base, menu.scratch + 4, 0); W32(base, menu.scratch + 8, 0);
  Call(ctx, base, sub_82886A90, menu.scratch);
}
float CursorY(int absolute_row) {
  return absolute_row * kCursorRowStep + (absolute_row >= kDisplayFirstRow ? kCursorGroupGap : 0) +
         (absolute_row >= kSoundFirstRow ? kCursorGroupGap : 0);
}
// The retail page update reads these each frame (82C71DE4); custom pages skip
// that update, so PlaceCursor repeats its positioning call.
void MoveCursor(uint8_t* base, int absolute_row) {
  W32(base, menu.object + kCursorTarget, std::bit_cast<uint32_t>(CursorY(absolute_row) - menu.cursor_bias));
  W32(base, menu.object + kCursorMoving, 1);
}
void PlaceCursor(PPCContext& ctx, uint8_t* base) {
  PPCContext call = ctx;
  call.r3.u64 = menu.object;
  call.r4.u64 = U32(base, menu.object + kCursorNode);
  call.f1.f64 = std::bit_cast<float>(U32(base, menu.object + kCursorTarget));
  sub_8293F530(call, base);
}
void Highlight(PPCContext& ctx, uint8_t* base, int row, bool selected) {
  Call(ctx, base, sub_82C40DD8, menu.object + 28, uint32_t(row), selected);
}
std::span<const Option> Options() {
  switch (menu.page) {
    case Page::Video: return kVideo;
    case Page::Misc: return kMisc;
    default: return {};
  }
}
const char* PageTitle() {
  switch (menu.page) {
    case Page::Video: return "VIDEO";
    case Page::Misc: return "MISC";
    default: return "OPTIONS";
  }
}
int RowCount() {
  return menu.page == Page::Categories ? int(std::size(kCategories)) : int(Options().size());
}
// The retail slot (0..5) that shows the selected row.
int Slot() { return menu.row - menu.scroll; }
// Scrolls just far enough to keep the selected row on screen.
void ScrollToRow() {
  menu.scroll = std::clamp(menu.scroll, std::max(0, menu.row - kVisibleRows + 1), menu.row);
}
bool OnRow(std::string_view key) {
  return menu.row < int(Options().size()) && Options()[menu.row].key == key;
}
bool IsDeferred(const Option& option) { return std::string_view(option.key) == "mgr_resolution_scale"; }
std::string RequestedScale() {
  return menu.pending_scale.empty() ? rex::cvar::GetFlagByName("mgr_resolution_scale") : menu.pending_scale;
}
void ApplyPendingScale() {
  if (menu.pending_scale.empty()) return;
  if (rex::cvar::SetFlagByName("mgr_resolution_scale", menu.pending_scale)) {
    menu.dirty = true;
    std::fprintf(stderr, "[mgr-native-options] set mgr_resolution_scale=%s\n", menu.pending_scale.c_str());
  }
  menu.pending_scale.clear();
}
// Double cvars read back as "1.000000"; compare those to "1" numerically.
bool IsChoice(const std::string& value, const char* choice) {
  if (value == choice) return true;
  char *value_end = nullptr, *choice_end = nullptr;
  const double a = std::strtod(value.c_str(), &value_end), b = std::strtod(choice, &choice_end);
  return value_end != value.c_str() && *value_end == '\0' && choice_end != choice && *choice_end == '\0' && a == b;
}
std::string Value(const Option& option) {
  const auto value = IsDeferred(option) ? RequestedScale() : rex::cvar::GetFlagByName(option.key);
  for (const Choice& c : option.choices) if (IsChoice(value, c.value)) return c.label;
  return value;
}
void Labels(uint8_t* base) {
  const uint32_t group = menu.object + 28;
  for (int i = 0; i < kVisibleRows; ++i) {
    std::string label, value;
    const int index = menu.scroll + i;
    if (menu.page == Page::Categories) {
      if (i < int(std::size(kCategories))) label = kCategories[i];
    } else if (index < int(Options().size())) {
      const auto& option = Options()[index]; label = option.label; value = Value(option);
    }
    const bool shown = !label.empty();
    Visible(base, group, U32(base, group + 40 + i * 4), shown);
    Visible(base, group, U32(base, group + 64 + i * 4), shown);
    SetText(base, group, U32(base, group + 64 + i * 4), label, 8);
    Visible(base, group, U32(base, group + 88 + i * 4), !value.empty());
    if (!value.empty()) SetText(base, group, U32(base, group + 88 + i * 4), value);
    Visible(base, group, U32(base, group + 112 + i * 4), menu.page != Page::Categories && index < int(Options().size()));
  }
  // Retail System rows 4 and 5 are 0-10 sliders (82C4DE90 / 82C4DB40) with ten
  // bar segments each at group+136..+212; custom rows are choices only.
  for (uint32_t k = 0; k < kSliderSegments; ++k) {
    const uint32_t id = U32(base, group + kSliderSegmentIds + k * 4);
    if (id) Visible(base, group, id, false);
  }
  SetText(base, group, 5, PageTitle(), 8);
  SetText(base, group, 6, PageTitle(), 8);
  const char* help = menu.page == Page::Categories ? "SELECT AN OPTIONS CATEGORY." :
      menu.row < int(Options().size()) ? Options()[menu.row].help : "";
  std::string wrapped(help);
  if (OnRow("mgr_resolution_scale")) {
    const auto status = MgrGetResolutionStatus();
    const auto resolution = [](const std::string& scale) {
      for (const Choice& c : kScale)
        if (scale == c.value) return c.label;
      return "UNAVAILABLE";
    };
    if (!menu.pending_scale.empty())
      wrapped = Value(kInternalResolution) + " applies when you close Options. Active: " + resolution(status.active) + ".";
    else
      wrapped = "Requested: " + Value(kInternalResolution) + ". Active: " + resolution(status.active) + ". " + status.state + ".";
    if (status.state == "failed") wrapped += " " + status.reason.substr(0, 55);
  }
  if (OnRow("mgr_render_target_precision") &&
      rex::cvar::GetFlagByName("mgr_render_target_precision") != MgrActiveRenderTargetPrecision())
    wrapped = "Restart the game to apply this rendering quality.";
  size_t start = 0;
  while (wrapped.size() - start > 52) {
    const size_t at = wrapped.rfind(' ', start + 52);
    if (at == std::string::npos || at < start) break;
    wrapped[at] = '\n'; start = at + 1;
  }
  SetText(base, menu.object, U32(base, menu.object + 864), wrapped);
}
// The controller diagram and its legend belong to the retail System page.
void ControllerDiagram(uint8_t* base, bool shown) {
  Visible(base, menu.object, 9, shown);
  for (uint32_t id = 12; id <= 34; ++id) Visible(base, menu.object, id, shown);
}
void CustomPage(PPCContext& ctx, uint8_t* base, Page page, int row = 0) {
  Restore(base);
  menu.page = page; menu.row = row; menu.scroll = 0; ScrollToRow();
  for (uint32_t offset : kGroupOffsets) {
    const bool first = offset == 28;
    Visible(base, menu.object, U32(base, menu.object + 848 + ((offset - 28) / 268) * 4), first);
  }
  ControllerDiagram(base, false);
  // A retail category leaves groups 1 and 2 active; custom pages use group 0.
  for (int i = 0; i < 3; ++i) Call(ctx, base, sub_82C40F08, menu.object + kGroupOffsets[i], i == 0);
  for (int i = 0; i < kVisibleRows; ++i) Highlight(ctx, base, i, false);
  Highlight(ctx, base, Slot(), true);
  Labels(base);
  MoveCursor(base, Slot());
  base[menu.object + 921] = uint8_t(Slot()); base[menu.object + 922] = 1;
  std::fprintf(stderr, "[mgr-native-options] page=%d row=%d\n", int(page), row);
}
void RetailPage(PPCContext& ctx, uint8_t* base, int category) {
  Restore(base); menu.page = Page::Retail;
  const int row = category == 0 ? 0 : category == 1 ? 6 : 9;
  base[menu.object + 921] = uint8_t(row); base[menu.object + 922] = 1;
  // Keep the retail view and handlers intact, including its original scrolling.
  for (int i = 0; i < 3; ++i) Call(ctx, base, sub_82C40F08, menu.object + kGroupOffsets[i], i == category);
  for (int i = 0; i < 12; ++i) {
    const int group = i < 6 ? 0 : i < 9 ? 1 : 2;
    Call(ctx, base, sub_82C40DD8, menu.object + kGroupOffsets[group], i, i == row);
  }
  // Show only this category: hide the other groups and lift this one into the
  // first group's slot. Restore() puts both back.
  for (int k = 0; k < 3; ++k)
    Visible(base, menu.object, U32(base, menu.object + kGroupContainers + k * 4), k == category);
  ControllerDiagram(base, category == 0);
  const uint32_t container = Node(base, menu.object, U32(base, menu.object + kGroupContainers + category * 4));
  if (category > 0 && container) {
    menu.lifted_node = container; menu.lifted_y = U32(base, container + kNodeY);
    menu.cursor_bias = CursorY(row);
    W32(base, container + kNodeY,
        std::bit_cast<uint32_t>(std::bit_cast<float>(menu.lifted_y) - menu.cursor_bias));
  }
  Call(ctx, base, sub_82C4C6A0, menu.object, row);
  MoveCursor(base, row);
  PlaceCursor(ctx, base);
  std::fprintf(stderr, "[mgr-native-options] retail category=%d row=%d lift=%.1f\n", category, row,
               menu.cursor_bias);
}
// Retail up/down crosses into the neighbouring group at its first/last row;
// wrap inside the shown category instead. In-group moves stay retail.
bool RetailWrap(PPCContext& ctx, uint8_t* base, bool up, bool down) {
  if (!up && !down) return false;
  constexpr int kFirst[] = {0, kDisplayFirstRow, kSoundFirstRow}, kLast[] = {kDisplayFirstRow - 1, kSoundFirstRow - 1, 11};
  const int row = int8_t(base[menu.object + 921]);
  const int first = kFirst[menu.category], last = kLast[menu.category];
  if (!((down && row == last) || (up && row == first))) return false;
  const int next = down ? first : last;
  const uint32_t group = menu.object + kGroupOffsets[menu.category];
  Call(ctx, base, sub_82C40DD8, group, uint32_t(row), 0);
  Call(ctx, base, sub_82C40DD8, group, uint32_t(next), 1);
  base[menu.object + 921] = uint8_t(next); base[menu.object + 922] = 1;
  MoveCursor(base, next);
  PlaceCursor(ctx, base);
  Call(ctx, base, sub_82C4C6A0, menu.object, uint32_t(next));
  Call(ctx, base, sub_82C46620, U32(base, menu.object + 840));
  Sound(ctx, base, 0x8218A538);
  std::fprintf(stderr, "[mgr-native-options] retail wrap row=%d\n", next);
  return true;
}
void FreeSession() {
  auto* memory = REX_KERNEL_STATE()->memory();
  for (auto& [_, t] : menu.text) if (t.allocation) memory->SystemHeapFree(t.allocation);
  if (menu.scratch) memory->SystemHeapFree(menu.scratch);
  menu.text.clear(); menu.visibility.clear(); menu.scratch = 0; menu.object = 0; menu.ready = false;
  menu.lifted_node = 0; menu.cursor_bias = 0;
}
void Save() {
  if (menu.dirty) { MgrSaveConfig(menu.config); menu.dirty = false; }
}
// MGR_NATIVE_OPTIONS=0 (diagnostic): leave the retail Options menu untouched.
bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MGR_NATIVE_OPTIONS");
    return !value || value[0] != '0';
  }();
  return enabled;
}
} // namespace

void MgrNativeOptionsConfigure(const std::filesystem::path& config_path) { menu.config = config_path; }

void MgrNativeOptionsBegin(PPCContext&, uint8_t*, uint32_t object) {
  if (!Enabled()) return;
  // A session that ended without the destructor hook leaves addresses that may
  // now belong to other objects: release them without writing guest memory.
  if (menu.object && menu.object != object) {
    std::fprintf(stderr, "[mgr-native-options] dropping stale session %08X\n", menu.object);
    FreeSession();
  }
  menu.object = object; menu.ready = false; menu.dirty = false; menu.page = Page::Categories; menu.row = menu.category = menu.scroll = 0;
  menu.pending_scale.clear();
}

bool MgrNativeOptionsUpdate(PPCContext& ctx, uint8_t* base, uint32_t object) {
  if (!Enabled() || object != menu.object) return false;
  // Phase 0 is the retail fade-in. It only ramps alpha (never node+944 or the
  // text contexts), so the custom page is installed before it is first drawn.
  const uint8_t phase = base[object + 920];
  if (!menu.ready) {
    // The UI resource may still be loading on the first frames.
    if (phase > 1 || !Node(base, object, U32(base, object + 848))) return false;
    menu.ready = true;
    CustomPage(ctx, base, Page::Categories);
    PlaceCursor(ctx, base);
    return phase == 1;
  }
  if (phase == 0) { Labels(base); return false; }  // retries text that was not ready yet
  if (phase != 1) return false;
  const uint32_t tapped = U32(base, kInput + 4), repeat = U32(base, kInput + 12);
  if ((tapped || repeat) && std::getenv("MGR_OPTIONS_TRACE"))
    std::fprintf(stderr, "[mgr-native-input] tap=%08X repeat=%08X\n", tapped, repeat);
  const bool up = repeat & (0x8 | 0x40000), down = repeat & (0x4 | 0x80000);
  if (menu.page == Page::Retail) return RetailWrap(ctx, base, up, down);
  const bool left = repeat & (0x1 | 0x10000), right = repeat & (0x2 | 0x20000);
  const bool confirm = tapped & 0x10, cancel = tapped & 0x20;
  if (cancel) {
    // Retail B (82C40728(obj, 0), phase 4 -> 5) then fades out over this page;
    // End() restores the retail text before the destructor runs.
    if (menu.page == Page::Categories) return false;
    Save(); CustomPage(ctx, base, Page::Categories, menu.category); Sound(ctx, base, 0x8218A524); return true;
  }
  if (up || down) {
    const int count = RowCount();
    Highlight(ctx, base, Slot(), false);
    menu.row = (menu.row + (down ? 1 : count - 1)) % count;
    ScrollToRow();
    std::fprintf(stderr, "[mgr-native-options] select page=%d row=%d slot=%d\n", int(menu.page), menu.row, Slot());
    Highlight(ctx, base, Slot(), true); base[object + 921] = uint8_t(Slot());
    MoveCursor(base, Slot());
    Sound(ctx, base, 0x8218A538);
  } else if (menu.page == Page::Categories && confirm) {
    menu.category = menu.row;
    if (menu.row < kFirstCustomCategory) RetailPage(ctx, base, menu.row);
    else CustomPage(ctx, base, kCustomPages[menu.row - kFirstCustomCategory]);
    Sound(ctx, base, 0x8218A660);
  } else if (menu.page != Page::Categories && menu.row < int(Options().size()) && (left || right)) {
    const auto& option = Options()[menu.row];
    const bool deferred = IsDeferred(option);
    const std::string current = deferred ? RequestedScale() : rex::cvar::GetFlagByName(option.key);
    size_t i = 0;
    for (size_t n = 0; n < option.choices.size(); ++n) if (IsChoice(current, option.choices[n].value)) i = n;
    const size_t next = (i + (right ? 1 : option.choices.size() - 1)) % option.choices.size();
    if (deferred) {
      const std::string_view value = option.choices[next].value;
      menu.pending_scale = value == rex::cvar::GetFlagByName(option.key) ? "" : std::string(value);
      Sound(ctx, base, 0x8218A538);
      std::fprintf(stderr, "[mgr-native-options] pending %s=%s\n", option.key, option.choices[next].value);
    } else if (rex::cvar::SetFlagByName(option.key, option.choices[next].value)) {
      menu.dirty = true; Sound(ctx, base, 0x8218A538);
      std::fprintf(stderr, "[mgr-native-options] set %s=%s\n", option.key, option.choices[next].value);
    }
  }
  if (menu.page != Page::Retail) Labels(base);
  PlaceCursor(ctx, base);
  Call(ctx, base, sub_82C46620, U32(base, object + 840));
  return true;
}

void MgrNativeOptionsAfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t object) {
  if (!Enabled() || menu.object != object || !menu.ready || menu.page != Page::Retail) return;
  if (base[object + 920] == 4) {
    base[object + 920] = 1;
    CustomPage(ctx, base, Page::Categories, menu.category);
  }
}

void MgrNativeOptionsEnd(uint8_t* base, uint32_t object) {
  if (!Enabled() || object != menu.object) return;
  ApplyPendingScale(); Save(); Restore(base);
  FreeSession();
}
