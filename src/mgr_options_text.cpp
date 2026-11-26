#include "mgr_options_text.h"

#include "mgr_options_guest.h"

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdio>
#include <cstring>

namespace mgr_options_text {
namespace {
using namespace mgr_options_guest;

constexpr uint32_t kTextVtable = 0x821CBF50;
// Widget pointer of a UI node (all node types), traced 2026-11-26.
constexpr uint32_t kNodeWidget = 1008;
constexpr uint32_t kLegendNode = 12;
// Shared core font bank pointer; bit 0x4000 of a glyph selects that bank's textures.
constexpr uint32_t kSharedFontBank = 0x83933450;
constexpr uint16_t kSharedBankFlag = 0x4000;
constexpr uint16_t kMaxAscii = 128;
constexpr size_t kMaxText = 180;
constexpr uint32_t kMaxLines = 3;
constexpr uint32_t kMcdMinSize = 40, kMcdMaxSize = 0x40000;
constexpr uint32_t kHeaderCopy = 64, kAllocSlack = 16384;
// Glyph word 0x8001 draws a space; 0x8000 ends a line.
constexpr uint16_t kGlyphSpace = 0x8001, kGlyphLineEnd = 0x8000;
constexpr uint32_t kLineStride = 24;

// Controller legend labels use the resident core font context. Found through a
// live retail widget rather than assuming a pool address.
uint32_t CoreContext(uint8_t* base, uint32_t menu_object) {
  const uint32_t legend = Node(base, menu_object, kLegendNode);
  if (!legend) return 0;
  const uint32_t widget = U32(base, legend + kNodeWidget);
  return widget && U32(base, widget) == kTextVtable ? U32(base, widget + 20) : 0;
}

void LoadGlyphs(uint8_t* base, NativeText& t, uint32_t source, uint16_t font) {
  // Retail labels use font 8 from the shared core bank, not the larger font 6
  // used by title text.
  const uint32_t shared = U32(base, kSharedFontBank);
  for (uint32_t i = 0; shared && i < U32(base, shared + 12); ++i) {
    const uint32_t symbol = shared + U32(base, shared + 8) + i * 8;
    if (U16(base, symbol) == 8 && U16(base, symbol + 2) < kMaxAscii)
      t.glyphs[char(U16(base, symbol + 2))] = uint16_t(U32(base, symbol + 4) | kSharedBankFlag);
  }
  if (font == 8) return;
  for (uint32_t i = 0; i < U32(base, source + 12); ++i) {
    const uint32_t symbol = source + U32(base, source + 8) + i * 8;
    if (U16(base, symbol) == font && U16(base, symbol + 2) < kMaxAscii)
      t.glyphs[char(U16(base, symbol + 2))] = uint16_t(U32(base, symbol + 4));
  }
}

// One paragraph with one line, rewritten by Fill for every new value.
void InitMessage(uint8_t* base, NativeText& t, uint32_t size) {
  t.paragraph = (size + 15) & ~15u;
  t.line = t.paragraph + 20;
  t.letters = t.line + 24 * 4;
  const uint32_t message = t.mcd + U32(base, t.mcd);
  W32(base, message, t.paragraph);
  W32(base, message + 4, 1);
  W32(base, t.mcd + t.paragraph, t.line);
  W32(base, t.mcd + t.paragraph + 4, 1);
  W32(base, t.mcd + t.paragraph + 8, 0);
  W32(base, t.mcd + t.paragraph + 12, 0);
  W32(base, t.mcd + t.paragraph + 16, uint32_t(t.font) << 16);
  W32(base, t.mcd + t.line, t.letters);
  W32(base, t.mcd + t.line + 4, 0);
  W32(base, t.mcd + t.line + 16, 39);
  W32(base, t.mcd + t.line + 20, 0);
}
}  // namespace

bool TextOverrides::Attach(uint8_t* base, uint32_t menu_object, uint32_t widget, uint16_t font) {
  const uint32_t context = U32(base, widget + 20);
  const uint32_t core_context = CoreContext(base, menu_object);
  const uint32_t source = core_context ? U32(base, core_context + 4) : 0;
  if (!context || !source) {
    std::fprintf(stderr, "[mgr-native-text] no source widget=%08X context=%08X core=%08X source=%08X\n", widget,
                 context, core_context, source);
    return false;
  }
  const uint32_t size = U32(base, source + 32) + U32(base, source + 36) * 8;
  if (size < kMcdMinSize || size > kMcdMaxSize) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::fprintf(stderr, "[mgr-native-text] bad size=%u core=%08X source=%08X header:", size, core_context, source);
      for (uint32_t off = 0; off < 40; off += 4) std::fprintf(stderr, " %08X", U32(base, source + off));
      std::fprintf(stderr, "\n");
    }
    return false;
  }
  NativeText& t = text_[widget];
  t.widget = widget;
  t.original_context = context;
  t.original_index = U32(base, widget + 168);
  t.original_line = U32(base, widget + 172);
  t.original_alignment = U32(base, widget + 12);
  t.font = font;
  t.allocation = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(size + kHeaderCopy + U32(base, source + 20) * 40 + kAllocSlack);
  if (!t.allocation) return false;
  std::memcpy(Guest(base, t.allocation), Guest(base, core_context), kHeaderCopy);
  t.mcd = t.allocation + kHeaderCopy;
  std::memcpy(Guest(base, t.mcd), Guest(base, source), size);
  W32(base, t.allocation + 4, t.mcd);
  LoadGlyphs(base, t, source, font);
  InitMessage(base, t, size);
  std::fprintf(stderr, "[mgr-native-options] text widget=%08X core=%08X glyphs=%zu\n", widget, source, t.glyphs.size());
  return true;
}

// Writes the letters of `value` (up to kMaxLines lines) into the private message.
void TextOverrides::Fill(uint8_t* base, NativeText& t, const std::string& value) {
  const std::string text = value.substr(0, kMaxText);
  uint32_t p = t.mcd + t.letters;
  uint32_t line_index = 0, line_count = 0, line_start = p;
  const auto finish_line = [&] {
    const uint32_t line = t.mcd + t.line + line_index * kLineStride;
    W16(base, p, kGlyphLineEnd);
    p += 2;
    W32(base, line, line_start - t.mcd);
    W32(base, line + 4, 0);
    W32(base, line + 8, line_count);
    W32(base, line + 12, line_count + 1);
    W32(base, line + 16, 39);
    W32(base, line + 20, 0);
    line_start = p;
    line_count = 0;
    ++line_index;
  };
  for (char c : text) {
    if (c == '\n' && line_index < kMaxLines) { finish_line(); continue; }
    const auto found = t.glyphs.find(c);
    const bool blank = c == ' ' || found == t.glyphs.end();
    W16(base, p, blank ? kGlyphSpace : found->second);
    W16(base, p + 2, blank ? t.font : 0);
    p += 4;
    ++line_count;
  }
  finish_line();
  W32(base, t.mcd + t.paragraph + 4, line_index);
}

bool TextOverrides::Set(uint8_t* base, uint32_t menu_object, uint32_t owner, uint32_t id, const std::string& value,
                        uint16_t font) {
  const uint32_t node = Node(base, owner, id);
  if (!node) return false;
  const uint32_t widget = U32(base, node + kNodeWidget);
  if (!widget || U32(base, widget) != kTextVtable) return false;
  if (!text_.count(widget) && !Attach(base, menu_object, widget, font)) {
    text_.erase(widget);
    return false;
  }
  NativeText& t = text_.at(widget);
  const bool value_row = owner != menu_object && font == kValueFont;
  if (t.shown == value && U32(base, widget + 20) == t.allocation) return true;
  if (U32(base, widget + 20) != t.allocation) {
    t.original_context = U32(base, widget + 20);
    t.original_index = U32(base, widget + 168);
    t.original_line = U32(base, widget + 172);
    t.original_alignment = U32(base, widget + 12);
  }
  Fill(base, t, value);
  if (value_row) W32(base, widget + 12, 0);
  W32(base, widget + 20, t.allocation);
  W32(base, widget + 168, 0);
  W32(base, widget + 172, 0);
  W32(base, widget + 184, 0);
  t.shown = value;
  return true;
}

void TextOverrides::Restore(uint8_t* base) {
  for (auto& [_, t] : text_) {
    if (U32(base, t.widget + 20) != t.allocation) continue;
    W32(base, t.widget + 20, t.original_context);
    W32(base, t.widget + 168, t.original_index);
    W32(base, t.widget + 172, t.original_line);
    W32(base, t.widget + 12, t.original_alignment);
    W32(base, t.widget + 184, 0);
    t.shown.clear();
  }
}

void TextOverrides::Free() {
  auto* memory = REX_KERNEL_STATE()->memory();
  for (auto& [_, t] : text_)
    if (t.allocation) memory->SystemHeapFree(t.allocation);
  text_.clear();
}

}  // namespace mgr_options_text
