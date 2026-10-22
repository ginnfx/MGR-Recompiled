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
#include <span>
#include <string>
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
constexpr float kCursorRowStep = 44.0f, kCursorGroupGap = 6.0f;
constexpr int kDisplayFirstRow = 6, kSoundFirstRow = 9;
enum class Page { Categories, Retail, Graphics, MoreGraphics, Recompilation };

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
const Option kOptions[] = {
  {"mgr_resolution_scale", "INTERNAL RESOLUTION", "Changes internal rendering detail. Applies after the GPU rebuilds its resources.", kScale},
  {"vsync", "VSYNC", "Synchronizes the guest retrace clock. High frame rate modes require this enabled.", kBool},
  {"mgr_frame_rate", "FRAME RATE", "60 FPS is the original timing. Higher rates are experimental.", kRate},
  {"vulkan_nonblocking_guest_present", "FRAME PACING", "Low latency avoids blocking the game while waiting for a display image.", kPacing},
  {"mgr_texture_filtering", "TEXTURE FILTERING", "Anisotropic filtering improves textures viewed at an angle.", kFilter},
  {"mgr_render_target_precision", "RENDERING QUALITY", "High preserves the original gamma precision. Changing this setting requires a restart.", kPrecision},
  {"mgr_film_grain", "FILM GRAIN", "Enables the original film grain post processing effect.", kBool},
  {"fullscreen", "FULLSCREEN", "Uses the current display in fullscreen mode.", kBool},
  {"mgr_output_resolution", "OUTPUT RESOLUTION", "Resizes the completed image. Internal rendering detail is controlled separately.", kOutput},
  {"present_letterbox", "PRESERVE ASPECT RATIO", "Preserves the original image proportions on wider or taller displays.", kBool},
};

constexpr Choice kPrompts[] = {{"AUTOMATIC", "auto"}, {"KEYBOARD", "keyboard"}, {"XBOX", "xbox"}, {"PLAYSTATION", "playstation"}};
constexpr Choice kSensitivity[] = {{"0.5", "0.5"}, {"1.0", "1"}, {"2.0", "2"}, {"3.0", "3"}};
const Option kRecompilation[] = {
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
  int row = 0, category = 0;
  bool ready = false, dirty = false;
  uint32_t scratch = 0;
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
  W32(base, menu.object + kCursorTarget, std::bit_cast<uint32_t>(CursorY(absolute_row)));
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
  if (menu.page == Page::Recompilation) return kRecompilation;
  return menu.page == Page::Graphics ? std::span(kOptions).first(5) : std::span(kOptions).subspan(5);
}
// mgr_settings keeps VSync on at 120 FPS and unlocked (guest vblank must stay
// a real retrace), so the row is fixed there.
bool VsyncFixedByFrameRate() { return rex::cvar::GetFlagByName("mgr_frame_rate") != "60"; }
std::string Value(const Option& option) {
  if (std::string_view(option.key) == "vsync" && VsyncFixedByFrameRate())
    return "ON (SET BY FRAME RATE)";
  const auto value = rex::cvar::GetFlagByName(option.key);
  for (const Choice& c : option.choices) if (value == c.value) return c.label;
  return value;
}
void Labels(uint8_t* base) {
  const uint32_t group = menu.object + 28;
  constexpr const char* categories[] = {"SYSTEM", "DISPLAY", "AUDIO", "GRAPHICS", "RECOMPILATION"};
  for (int i = 0; i < 6; ++i) {
    std::string label, value;
    if (menu.page == Page::Categories) {
      if (i < 5) label = categories[i];
    } else if (i < int(Options().size())) {
      const auto& option = Options()[i]; label = option.label; value = Value(option);
    } else if (menu.page != Page::Recompilation)
      label = menu.page == Page::Graphics ? "MORE GRAPHICS" : "PREVIOUS GRAPHICS";
    const bool shown = !label.empty();
    Visible(base, group, U32(base, group + 40 + i * 4), shown);
    Visible(base, group, U32(base, group + 64 + i * 4), shown);
    SetText(base, group, U32(base, group + 64 + i * 4), label, 8);
    Visible(base, group, U32(base, group + 88 + i * 4), !value.empty());
    if (!value.empty()) SetText(base, group, U32(base, group + 88 + i * 4), value);
    Visible(base, group, U32(base, group + 112 + i * 4), menu.page != Page::Categories && i < int(Options().size()));

  }
  SetText(base, group, 5, menu.page == Page::Categories ? "OPTIONS" : menu.page == Page::Recompilation ? "RECOMPILATION" : "GRAPHICS", 8);
  SetText(base, group, 6, menu.page == Page::Categories ? "OPTIONS" : menu.page == Page::Recompilation ? "RECOMPILATION" : "GRAPHICS", 8);
  const char* help = menu.page == Page::Categories ? "SELECT AN OPTIONS CATEGORY." :
      menu.row < int(Options().size()) ? Options()[menu.row].help : "SELECT TO VIEW THE OTHER GRAPHICS SETTINGS.";
  std::string wrapped(help);
  if (menu.page == Page::Graphics && menu.row == 0) {
    const auto status = MgrGetResolutionStatus();
    const auto resolution = [](const std::string& scale) {
      for (const Choice& c : kScale)
        if (scale == c.value) return c.label;
      return "UNAVAILABLE";
    };
    wrapped = "Requested: " + Value(kOptions[0]) + ". Active: " + resolution(status.active) + ". " + status.state + ".";
    if (status.state == "failed") wrapped += " " + status.reason.substr(0, 55);
  }
  if (menu.row < int(Options().size()) && menu.page != Page::Categories &&
      std::string_view(Options()[menu.row].key) == "vsync" && VsyncFixedByFrameRate())
    wrapped = "VSync stays on at 120 FPS and Unlocked. Set Frame Rate to 60 FPS to change it.";
  if (menu.page == Page::MoreGraphics && menu.row == 0 &&
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
void CustomPage(PPCContext& ctx, uint8_t* base, Page page, int row = 0) {
  Restore(base);
  menu.page = page; menu.row = row;
  for (uint32_t offset : kGroupOffsets) {
    const bool first = offset == 28;
    Visible(base, menu.object, U32(base, menu.object + 848 + ((offset - 28) / 268) * 4), first);
  }
  // The controller diagram belongs to the retail Controls page.
  Visible(base, menu.object, 9, false);
  for (uint32_t id = 12; id <= 34; ++id) Visible(base, menu.object, id, false);
  // A retail category leaves groups 1 and 2 active; custom pages use group 0.
  for (int i = 0; i < 3; ++i) Call(ctx, base, sub_82C40F08, menu.object + kGroupOffsets[i], i == 0);
  for (int i = 0; i < 6; ++i) Highlight(ctx, base, i, false);
  Highlight(ctx, base, row, true);
  Labels(base);
  MoveCursor(base, row);
  base[menu.object + 921] = uint8_t(row); base[menu.object + 922] = 1;
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
  Call(ctx, base, sub_82C4C6A0, menu.object, row);
  MoveCursor(base, row);
  std::fprintf(stderr, "[mgr-native-options] retail category=%d row=%d\n", category, row);
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
  menu.object = object; menu.ready = false; menu.dirty = false; menu.page = Page::Categories; menu.row = menu.category = 0;
}

bool MgrNativeOptionsUpdate(PPCContext& ctx, uint8_t* base, uint32_t object) {
  if (!Enabled() || object != menu.object || base[object + 920] != 1) return false;
  if (!menu.ready) { menu.ready = true; CustomPage(ctx, base, Page::Categories); return true; }
  if (menu.page == Page::Retail) return false;
  const uint32_t tapped = U32(base, kInput + 4), repeat = U32(base, kInput + 12);
  if ((tapped || repeat) && std::getenv("MGR_OPTIONS_TRACE"))
    std::fprintf(stderr, "[mgr-native-input] tap=%08X repeat=%08X\n", tapped, repeat);
  const bool up = repeat & (0x8 | 0x40000), down = repeat & (0x4 | 0x80000);
  const bool left = repeat & (0x1 | 0x10000), right = repeat & (0x2 | 0x20000);
  const bool confirm = tapped & 0x10, cancel = tapped & 0x20;
  if (cancel) {
    if (menu.page == Page::Categories) { Restore(base); return false; }
    Save(); CustomPage(ctx, base, Page::Categories, menu.category); Sound(ctx, base, 0x8218A524); return true;
  }
  if (up || down) {
    const int count = menu.page == Page::Categories ? 5 : menu.page == Page::Recompilation ? int(Options().size()) : 6;
    Highlight(ctx, base, menu.row, false);
    menu.row = (menu.row + (down ? 1 : count - 1)) % count;
    std::fprintf(stderr, "[mgr-native-options] select page=%d row=%d\n", int(menu.page), menu.row);
    Highlight(ctx, base, menu.row, true); base[object + 921] = uint8_t(menu.row);
    MoveCursor(base, menu.row);
    Sound(ctx, base, 0x8218A538);
  } else if (menu.page == Page::Categories && confirm) {
    menu.category = menu.row;
    if (menu.row == 3) CustomPage(ctx, base, Page::Graphics);
    else if (menu.row == 4) CustomPage(ctx, base, Page::Recompilation);
    else RetailPage(ctx, base, menu.row);
    Sound(ctx, base, 0x8218A660);
  } else if ((menu.page == Page::Graphics || menu.page == Page::MoreGraphics) && menu.row == 5 && (confirm || left || right)) {
    CustomPage(ctx, base, menu.page == Page::Graphics ? Page::MoreGraphics : Page::Graphics);
    Sound(ctx, base, 0x8218A660);
  } else if (menu.page != Page::Categories && menu.row < int(Options().size()) && (left || right)) {
    const auto& option = Options()[menu.row];
    const std::string current = rex::cvar::GetFlagByName(option.key);
    if (std::string_view(option.key) == "vsync" && VsyncFixedByFrameRate()) {
      Sound(ctx, base, 0x8218A524); return true;
    }
    size_t i = 0;
    for (size_t n = 0; n < option.choices.size(); ++n) if (current == option.choices[n].value) i = n;
    const size_t next = (i + (right ? 1 : option.choices.size() - 1)) % option.choices.size();
    if (rex::cvar::SetFlagByName(option.key, option.choices[next].value)) {
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
  Save(); Restore(base);
  auto* memory = REX_KERNEL_STATE()->memory();
  for (auto& [_, t] : menu.text) if (t.allocation) memory->SystemHeapFree(t.allocation);
  if (menu.scratch) memory->SystemHeapFree(menu.scratch);
  menu.text.clear(); menu.visibility.clear(); menu.scratch = 0; menu.object = 0; menu.ready = false;
}
