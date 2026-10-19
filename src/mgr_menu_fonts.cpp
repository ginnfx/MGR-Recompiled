#include "mgr_menu_fonts.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
ImFont* labels = nullptr;
ImFont* values = nullptr;
struct Glyph {
  uint32_t family, code;
  uint16_t width, height;
  float advance, line;
  std::vector<uint8_t> alpha;
  ImFontAtlasRectId rect;
};
template <typename T> bool Read(std::ifstream& stream, T& value) {
  return bool(stream.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
std::pair<int, int> InkRows(const Glyph& g) {
  int top = g.height, bottom = 0;
  for (int y = 0; y < g.height; ++y)
    for (int x = 0; x < g.width; ++x)
      if (g.alpha[y * g.width + x] > 16) {
        top = std::min(top, y);
        bottom = std::max(bottom, y + 1);
      }
  return {top, bottom};
}
}

void MgrConfigureMenuFonts(ImFontAtlas* atlas, const std::filesystem::path& game_root) {
  labels = values = nullptr;
  // Prepared from the user's game data by tools/assets/prepare_ui_font.py.
  auto path = rex::filesystem::GetExecutableFolder() / "mgr_ui_font.bin";
  if (!std::filesystem::exists(path)) path = game_root.parent_path() / "mgr_ui_font.bin";
  std::ifstream stream(path, std::ios::binary);
  std::array<char, 8> magic{};
  uint32_t count = 0;
  if (!Read(stream, magic) || std::memcmp(magic.data(), "MGRFONT2", 8) ||
      !Read(stream, count) || count > 512) {
    std::fprintf(stderr, "[mgr-options] menu font asset missing/invalid: %s\n", path.c_str());
    return;
  }
  std::vector<Glyph> glyphs(count);
  for (auto& g : glyphs) {
    if (!Read(stream, g.family) || !Read(stream, g.code) || !Read(stream, g.width) ||
        !Read(stream, g.height) || !Read(stream, g.advance) || !Read(stream, g.line) ||
        (g.family != 3 && g.family != 6) || g.code < 33 || g.code > 126 ||
        !g.width || !g.height || g.width > 256 || g.height > 128 ||
        !(g.advance > 0 && g.advance < 256)) return;
    g.alpha.resize(size_t(g.width) * g.height);
    if (!stream.read(reinterpret_cast<char*>(g.alpha.data()), g.alpha.size())) return;
  }
  // English message archives contain only the extended face's used letters.
  // Fill absent characters (notably J, X, punctuation) with the game's value
  // face, matching cap height/baseline/width instead of showing debug-font glyphs.
  const auto find = [&](uint32_t family, uint32_t code) -> const Glyph* {
    for (const auto& g : glyphs) if (g.family == family && g.code == code) return &g;
    return nullptr;
  };
  if (find(3, 'A') && find(6, 'A')) {
    const Glyph reference = *find(6, 'A');
    const float stretch = reference.advance / find(3, 'A')->advance;
    const auto [cap_top, cap_bottom] = InkRows(reference);
    const auto original_count = glyphs.size();
    for (size_t i = 0; i < original_count; ++i) {
      const Glyph source = glyphs[i];
      if (source.family != 3 || find(6, source.code)) continue;
      const auto [top, bottom] = InkRows(source);
      if (top >= bottom || cap_top >= cap_bottom) continue;
      Glyph g = source;
      g.family = 6;
      g.height = reference.height;
      g.width = uint16_t(std::max(1l, std::lround(source.width * stretch)));
      g.advance = source.advance * stretch;
      g.alpha.assign(size_t(g.width) * g.height, 0);
      for (int y = cap_top; y < cap_bottom; ++y) {
        const int sy = top + (y - cap_top) * (bottom - top) / (cap_bottom - cap_top);
        for (int x = 0; x < g.width; ++x)
          g.alpha[y * g.width + x] = source.alpha[sy * source.width + x * source.width / g.width];
      }
      glyphs.push_back(std::move(g));
    }
  }
  ImFontConfig cfg;
  cfg.SizePixels = 39;
  cfg.PixelSnapH = false;
  values = atlas->AddFontDefault(&cfg);
  cfg.SizePixels = 58;
  labels = atlas->AddFontDefault(&cfg);
  atlas->Build();
  for (auto& g : glyphs) {
    auto* font = g.family == 6 ? labels : values;
    g.rect = atlas->AddCustomRectFontGlyph(font, g.code, g.width, g.height, g.advance);
    // Bitmap glyphs are white masks tinted by MGR's normal/selected colors.
    auto* glyph = const_cast<ImFontGlyph*>(font->GetFontBaked(font->LegacySize)->FindGlyph(g.code));
    if (glyph) glyph->Colored = false;
  }
  // A fallback TTF's space would be much wider than the original game font.
  for (auto* font : {values, labels}) {
    const auto id = atlas->AddCustomRectFontGlyph(font, ' ', 1, 1, font == labels ? 18 : 10);
    ImFontAtlasRect rect;
    if (atlas->GetCustomRect(id, &rect))
      std::memset(atlas->TexData->GetPixelsAt(rect.x, rect.y), 0, atlas->TexData->BytesPerPixel);
  }
  // Add every rectangle before retaining pixel pointers: atlas growth can move it.
  unsigned char* rgba;
  int width, height;
  atlas->GetTexDataAsRGBA32(&rgba, &width, &height);
  for (const auto& g : glyphs) {
    ImFontAtlasRect rect;
    if (!atlas->GetCustomRect(g.rect, &rect)) continue;
    for (unsigned y = 0; y < g.height; ++y) {
      for (unsigned x = 0; x < g.width; ++x) {
        auto* p = rgba + ((rect.y + y) * width + rect.x + x) * 4;
        p[0] = p[1] = p[2] = 255;
        p[3] = g.alpha[y * g.width + x];
      }
    }
  }
  // Reuse the original bitmap at every window size; never rasterize TTF
  // substitutes at a new ImGui baked size on Retina/window changes.
  labels->Flags |= ImFontFlags_LockBakedSizes;
  values->Flags |= ImFontFlags_LockBakedSizes;
  std::fprintf(stderr, "[mgr-options] loaded %u original menu glyphs\n", count);
}

ImFont* MgrMenuLabelFont() { return labels; }
ImFont* MgrMenuValueFont() { return values; }
