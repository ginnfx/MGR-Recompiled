#include "mgr_options_pictures.h"

#include "mgr_picture_canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

namespace mgr_pictures {
namespace {

constexpr Color kInk{0.62f, 0.93f, 1.0f, 1.0f};
constexpr Color kDim{0.20f, 0.40f, 0.50f, 1.0f};
constexpr Color kPanel{0.02f, 0.07f, 0.10f, 0.78f};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kWarm{1.0f, 0.62f, 0.25f, 1.0f};
constexpr float kPi = 3.14159265f;
// Opacity of a fully covered PixelGrid cell; slightly below solid so the art does not glare.
constexpr float kCellFill = 0.82f;
// Scenes are laid out on a 400x320 stage.
constexpr int kStageWidth = 400, kStageHeight = 320;
// The stage sits in a square panel, scaled up to kPictureSize.
constexpr int kPanelWidth = 400, kPanelHeight = 400;

bool IsOn(std::string_view value) { return value == "true" || value == "1"; }
double Number(std::string_view value, double fallback = 0) {
  char* end = nullptr;
  const std::string text(value);
  const double v = std::strtod(text.c_str(), &end);
  return end == text.c_str() ? fallback : v;
}
Color Gray(float v, float a = 1.0f) { return {v, v, v, a}; }
Color Tint(Color c, float a) { return {c.r, c.g, c.b, c.a * a}; }

void Arrow(Canvas& c, Point a, Point b, Color color) {
  c.Line(a, b, 4, color);
  const float angle = std::atan2(b.y - a.y, b.x - a.x);
  c.Polygon({b, {b.x - 16 * std::cos(angle - 0.45f), b.y - 16 * std::sin(angle - 0.45f)},
             {b.x - 16 * std::cos(angle + 0.45f), b.y - 16 * std::sin(angle + 0.45f)}}, color);
}

// On/off switch under the diagram of a yes/no row.
void Toggle(Canvas& c, float cx, float cy, bool on) {
  c.RoundRect(cx - 34, cy - 14, 68, 28, 14, on ? kInk : kDim);
  c.Disc(on ? cx + 20 : cx - 20, cy, 10, on ? kPanel : kInk);
}

void Monitor(Canvas& c, float x, float y, float w, float h, Color fill) {
  c.RoundRect(x, y, w, h, 6, kInk);
  c.Rect(x + 6, y + 6, w - 12, h - 12, fill);
  c.Rect(x + w / 2 - 6, y + h, 12, 18, kDim);
  c.RoundRect(x + w / 2 - 36, y + h + 16, 72, 8, 4, kDim);
}

// A shape shown through square "pixels" of the given size, to compare sampling.
template <typename Coverage>
void PixelGrid(Canvas& c, float x, float y, float w, float h, float cell, Coverage coverage) {
  for (float py = y; py + cell <= y + h + 0.01f; py += cell)
    for (float px = x; px + cell <= x + w + 0.01f; px += cell) {
      const float v = coverage(px + cell / 2, py + cell / 2, cell);
      if (v > 0.01f) c.Rect(px, py, cell, cell, Tint(kInk, v * kCellFill));
    }
}

// Faint lines between the "pixels" of a PixelGrid, so each cell reads as one sample.
void GridLines(Canvas& c, float x, float y, float w, float h, float cell) {
  const Color line = Tint(kPanel, 0.55f);
  for (float gx = x + cell; gx < x + w - 0.01f; gx += cell) c.Rect(gx - 0.75f, y, 1.5f, h, line);
  for (float gy = y + cell; gy < y + h - 0.01f; gy += cell) c.Rect(x, gy - 0.75f, w, 1.5f, line);
}

// Fraction of a cell covered by the half plane left of the line x - y * slope < offset.
float EdgeCoverage(float cx, float cy, float cell, float slope, float offset) {
  int inside = 0;
  for (int sy = 0; sy < 8; ++sy)
    for (int sx = 0; sx < 8; ++sx) {
      const float x = cx + ((sx + 0.5f) / 8 - 0.5f) * cell, y = cy + ((sy + 0.5f) / 8 - 0.5f) * cell;
      inside += x - y * slope < offset;
    }
  return inside / 64.0f;
}
float Quantise(float v, int levels) { return levels <= 1 ? (v >= 0.5f ? 1.0f : 0.0f) : std::round(v * levels) / levels; }

void Fullscreen(Canvas& c, std::string_view value) {
  const bool on = IsOn(value);
  Monitor(c, 80, 50, 240, 150, kDim);
  if (on) {
    c.Rect(86, 56, 228, 138, Tint(kInk, 0.55f));
  } else {
    c.RoundRect(118, 78, 160, 96, 4, kInk);
    c.Rect(118, 78, 160, 14, kInk);
    c.Rect(122, 96, 152, 74, kPanel);
  }
  Toggle(c, 200, 262, on);
}

void OutputResolution(Canvas& c, std::string_view value) {
  const float scale = value == "source" ? 1.0f : value == "display" ? 1.9f : value == "1920x1080" ? 1.45f : 1.7f;
  c.Frame(40, 130, 100, 56, 4, kInk);
  c.Rect(48, 138, 84, 40, Tint(kInk, 0.35f));
  Arrow(c, {150, 158}, {196, 158}, kInk);
  const float w = 100 * scale, h = 56 * scale;
  c.Frame(205, 158 - h / 2, w, h, 4, kInk);
  c.Rect(213, 166 - h / 2, w - 16, h - 16, Tint(kInk, 0.35f));
}

void InternalResolution(Canvas& c, std::string_view value) {
  const float scale = float(Number(value, 1));
  const float cell = 28.0f / scale;
  const float cx = 200, cy = 150, radius = 96;
  PixelGrid(c, 100, 50, 200, 200, cell, [&](float x, float y, float) {
    return std::hypot(x - cx, y - cy) < radius ? 1.0f : 0.0f;
  });
  GridLines(c, 100, 50, 200, 200, cell);
  c.Ring(cx, cy, radius, 2, Tint(kWhite, 0.5f));
}

void Upscaling(Canvas& c, std::string_view value) {
  const bool on = value != "off";
  const auto circle = [](float cx, float cy, float r) {
    return [=](float x, float y, float) { return std::hypot(x - cx, y - cy) < r ? 1.0f : 0.0f; };
  };
  // The low resolution render, then the same picture enlarged: blocky without
  // upscaling, rebuilt with fine detail when it is on.
  PixelGrid(c, 36, 102, 96, 96, 12.0f, circle(84, 150, 44));
  GridLines(c, 36, 102, 96, 96, 12.0f);
  Arrow(c, {150, 150}, {206, 150}, on ? kWhite : kDim);
  if (on) {
    c.Disc(286, 150, 56, Tint(kInk, kCellFill));
    c.Ring(286, 150, 56, 2, Tint(kWhite, 0.5f));
    c.Disc(268, 128, 5, kWhite);
  } else {
    PixelGrid(c, 216, 80, 140, 140, 20.0f, circle(286, 150, 56));
    GridLines(c, 216, 80, 140, 140, 20.0f);
  }
}

void AntiAliasing(Canvas& c, std::string_view value) {
  const int samples = int(Number(value));
  constexpr float kCell = 16.0f, kX = 56, kY = 40, kW = 288, kH = 240;
  PixelGrid(c, kX, kY, kW, kH, kCell, [&](float x, float y, float cell) {
    return Quantise(EdgeCoverage(x, y, cell, 0.55f, 170), samples);
  });
  GridLines(c, kX, kY, kW, kH, kCell);
  c.Line({170 + 0.55f * kY, kY}, {170 + 0.55f * (kY + kH), kY + kH}, 2, Tint(kWhite, 0.6f));
}

void TransparencyAa(Canvas& c, std::string_view value) {
  const bool smooth = IsOn(value);
  PixelGrid(c, 60, 40, 280, 190, 14.0f, [&](float x, float y, float cell) {
    // Thin diagonal bars like a chain-link fence, thinner than a pixel.
    const float phase = std::fmod(x + y * 0.5f, 56.0f);
    const float coverage = std::clamp(1.0f - std::fabs(phase - 28.0f) / (cell * 0.9f), 0.0f, 1.0f);
    return smooth ? Quantise(coverage, 4) : (coverage > 0.5f ? 1.0f : 0.0f);
  });
  GridLines(c, 60, 40, 280, 190, 14.0f);
  Toggle(c, 200, 270, smooth);
}

void ShadowResolution(Canvas& c, std::string_view value) {
  const float step = value == "4096" ? 6.0f : value == "2048" ? 12.0f : 24.0f;
  c.Rect(40, 200, 320, 8, kDim);
  c.Rect(60, 90, 70, 110, kInk);
  PixelGrid(c, 130, 208, 224, 64, step, [&](float x, float y, float) {
    // A slanted shadow edge on the floor.
    return x - 130 < 170 - (y - 208) * 1.1f ? 0.7f : 0.0f;
  });
  GridLines(c, 130, 208, 224, 64, step);
}

void AspectRatio(Canvas& c, std::string_view value) {
  const bool keep = IsOn(value);
  c.Frame(30, 90, 340, 142, 4, kDim);
  if (keep) {
    c.Rect(122, 98, 156, 126, Tint(kInk, 0.35f));
    c.Disc(200, 161, 38, kInk);
  } else {
    c.Rect(38, 98, 324, 126, Tint(kInk, 0.35f));
    c.Shape(120, 125, 280, 197, kInk, [](float px, float py) {
      const float dx = (px - 200) / 80, dy = (py - 161) / 38;
      return (std::hypot(dx, dy) - 1.0f) * 38;
    });
  }
  Toggle(c, 200, 270, keep);
}

void Vsync(Canvas& c, std::string_view value) {
  const bool on = IsOn(value);
  const float tear = on ? 0.0f : 34.0f;
  for (int i = 0; i < 6; ++i) {
    const float x = 70 + i * 48;
    c.Rect(x, 50, 24, 90, Tint(kInk, 0.9f));
    c.Rect(x + tear, 140, 24, 90, Tint(kInk, 0.9f));
  }
  if (!on) c.Line({50, 140}, {350, 140}, 3, kWarm);
  Toggle(c, 200, 270, on);
}

void FrameRate(Canvas& c, std::string_view value) {
  const int ticks = value == "60" ? 6 : value == "120" ? 12 : 24;
  c.Line({40, 200}, {360, 200}, 3, kDim);
  for (int i = 0; i <= ticks; ++i) {
    const float t = float(i) / float(ticks);
    const float x = 40 + 320 * t, y = 190 - 110 * std::sin(t * kPi);
    c.Line({x, 200}, {x, 212}, 3, kDim);
    c.Disc(x, y, ticks > 12 ? 5.0f : 8.0f, kInk);
  }
}

void Brightness(Canvas& c, std::string_view value) {
  const float level = float(Number(value, 1));
  for (int i = 0; i < 8; ++i) {
    const float base = (float(i) + 0.5f) / 8.0f;
    c.Rect(50 + i * 38.0f, 150, 38, 100, Gray(std::clamp(std::pow(base, 1.0f / level), 0.0f, 1.0f)));
  }
  const float glow = std::clamp((level - 0.7f) / 0.6f, 0.0f, 1.0f);
  c.Disc(200, 85, 34, Gray(0.3f + 0.7f * glow));
  for (int i = 0; i < 8; ++i) {
    const float a = float(i) * kPi / 4;
    c.Line({200 + 46 * std::cos(a), 85 + 46 * std::sin(a)}, {200 + 62 * std::cos(a), 85 + 62 * std::sin(a)}, 4,
           Tint(kInk, 0.3f + 0.7f * glow));
  }
}

void Sharpening(Canvas& c, std::string_view value) {
  const float amount = float(Number(value));
  const float blur = 22.0f * (1.0f - 0.7f * amount) + 2.0f;
  const auto smooth = [](float t) { t = std::clamp(t * 0.5f + 0.5f, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
  for (int x = 60; x < 340; ++x) {
    const float d = float(x) - 200.0f;
    const float base = smooth(d / blur);
    const float v = std::clamp(base + amount * 0.6f * (base - smooth(d / (blur * 3.0f))), 0.0f, 1.0f);
    c.Rect(float(x), 60, 1, 150, Gray(0.12f + 0.8f * v));
  }
  c.Disc(130, 250, 12, Gray(0.2f));
  c.Disc(270, 250, 12, Gray(0.92f));
}

void TextureFiltering(Canvas& c, std::string_view value) {
  const double aniso = value == "off" ? 0.5 : value == "game" ? 2.0 : Number(value.empty() ? value : value.substr(0, value.size() - 1), 2);
  constexpr int kTop = 60, kRows = 200;
  for (int r = 0; r < kRows; ++r) {
    const float t = float(r) / float(kRows);
    const float z = 1.0f / (0.08f + t);  // distance of this floor row
    // Far rows lose contrast (blur toward grey) unless anisotropic filtering keeps it.
    const float contrast = std::clamp(float(aniso) * 0.9f / z, 0.05f, 1.0f);
    for (int x = 40; x < 360; ++x) {
      const float u = (float(x) - 200.0f) * z * 0.02f;
      const float pattern = std::clamp(std::sin(u * kPi) * std::sin(z * 0.8f * kPi) * 3.0f, -1.0f, 1.0f);
      c.Blend(x, kTop + r, Gray(0.14f + 0.78f * (0.5f + 0.5f * pattern * contrast)), 1.0f);
    }
  }
}

void ModelDetail(Canvas& c, std::string_view value) {
  const int far_sides = value == "max" ? 48 : value == "high" ? 14 : 6;
  c.RegularPolygon(120, 170, 80, 48, 0.3f, Tint(kInk, 0.9f));
  c.RegularPolygon(290, 130, 34, far_sides, 0.3f, Tint(kInk, 0.9f));
  c.Line({40, 250}, {360, 250}, 3, kDim);
  Arrow(c, {170, 250}, {250, 250}, kDim);
}

void Bloom(Canvas& c, std::string_view value) {
  const bool on = IsOn(value);
  if (on)
    for (int i = 10; i >= 1; --i) c.Disc(200, 150, 28.0f + 12.0f * float(i), Tint(kInk, 0.045f));
  c.Disc(200, 150, 26, kWhite);
  c.Rect(60, 220, 280, 6, Tint(kInk, 0.5f));
  Toggle(c, 200, 270, on);
}

void FilmGrain(Canvas& c, std::string_view value) {
  const bool on = IsOn(value);
  for (int y = 50; y < 230; y += 3)
    for (int x = 60; x < 340; x += 3) {
      uint32_t h = uint32_t(x * 73856093) ^ uint32_t(y * 19349663);
      h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
      const float n = on ? float(h & 255) / 255.0f - 0.5f : 0.0f;
      c.Rect(float(x), float(y), 3, 3, Gray(std::clamp(0.45f + 0.5f * n, 0.0f, 1.0f)));
    }
  Toggle(c, 200, 270, on);
}

void ButtonPrompts(Canvas& c, std::string_view value) {
  const auto keys = [&](float x, float y) {
    for (int i = 0; i < 3; ++i) c.RoundRect(x + 52.0f * float(i), y, 44, 44, 6, kInk);
    c.RoundRect(x, y + 52, 148, 36, 6, kInk);
  };
  const auto pad = [&](float x, float y, bool playstation) {
    const std::array<Point, 4> at{{{x + 44, y}, {x + 88, y + 44}, {x + 44, y + 88}, {x, y + 44}}};
    const std::array<Color, 4> xbox{{{1, 0.9f, 0.2f, 1}, {1, 0.3f, 0.3f, 1}, {0.4f, 0.9f, 0.4f, 1}, {0.4f, 0.6f, 1, 1}}};
    for (int i = 0; i < 4; ++i) {
      c.Disc(at[i].x, at[i].y, 22, playstation ? kDim : Tint(xbox[i], 0.9f));
      if (playstation) {
        if (i == 0) c.RegularPolygon(at[i].x, at[i].y, 12, 3, -kPi / 2, kInk);
        if (i == 1) c.Ring(at[i].x, at[i].y, 10, 4, kInk);
        if (i == 2) { c.Line({at[i].x - 9, at[i].y - 9}, {at[i].x + 9, at[i].y + 9}, 4, kInk); c.Line({at[i].x + 9, at[i].y - 9}, {at[i].x - 9, at[i].y + 9}, 4, kInk); }
        if (i == 3) c.Frame(at[i].x - 10, at[i].y - 10, 20, 20, 4, kInk);
      }
    }
  };
  if (value == "keyboard") keys(126, 100);
  else if (value == "xbox") pad(150, 80, false);
  else if (value == "playstation") pad(150, 80, true);
  else { keys(30, 120); pad(250, 100, false); }
}

void MouseCamera(Canvas& c, std::string_view value) {
  const bool on = IsOn(value);
  c.RoundRect(160, 60, 80, 130, 38, on ? kInk : kDim);
  c.Line({200, 60}, {200, 112}, 3, kPanel);
  c.Rect(160, 112, 80, 3, kPanel);
  if (on) {
    Arrow(c, {260, 150}, {330, 150}, kInk);
    Arrow(c, {140, 150}, {70, 150}, kInk);
  }
  Toggle(c, 200, 262, on);
}

void MouseSensitivity(Canvas& c, std::string_view value) {
  const float sensitivity = float(Number(value, 1));
  c.RoundRect(60, 96, 40, 64, 18, kDim);
  c.Line({90, 128}, {110, 128}, 2, kDim);
  Arrow(c, {120, 128}, {120 + 60 * std::sqrt(sensitivity), 128}, kInk);
  c.Ring(250, 190, 24 + 18 * sensitivity, 4, Tint(kInk, 0.8f));
  c.Disc(250, 190, 6, kWhite);
}

void SkipLogos(Canvas& c, std::string_view value) {
  const bool skip = IsOn(value);
  for (int i = 0; i < 3; ++i) {
    c.Frame(50.0f + 100.0f * float(i), 100, 80, 60, 4, skip ? kDim : kInk);
    c.Disc(90.0f + 100.0f * float(i), 130, 14, skip ? kDim : kInk);
  }
  if (skip) c.Line({40, 180}, {360, 80}, 8, kWarm);
  Toggle(c, 200, 250, skip);
}

struct Scene { std::string_view key; void (*draw)(Canvas&, std::string_view); };
constexpr Scene kScenes[] = {
    {"fullscreen", Fullscreen},
    {"mgr_output_resolution", OutputResolution},
    {"mgr_resolution_scale", InternalResolution},
    {"mgr_upscaler", Upscaling},
    {"mgr_msaa", AntiAliasing},
    {"mgr_transparency_aa", TransparencyAa},
    {"mgr_shadow_resolution", ShadowResolution},
    {"present_letterbox", AspectRatio},
    {"mgr_vsync", Vsync},
    {"mgr_frame_rate", FrameRate},
    {"mgr_brightness", Brightness},
    {"mgr_sharpening", Sharpening},
    {"mgr_texture_filtering", TextureFiltering},
    {"mgr_lod", ModelDetail},
    {"mgr_bloom", Bloom},
    {"mgr_film_grain", FilmGrain},
    {"mgr_prompt_style", ButtonPrompts},
    {"mnk_mouse", MouseCamera},
    {"mnk_sensitivity", MouseSensitivity},
    {"mgr_skip_intro_logos", SkipLogos},
};

}  // namespace

std::vector<uint8_t> RenderOptionPicture(std::string_view key, std::string_view value) {
  // Scenes are drawn on a 400x320 stage, centred in a 400x400 panel, then scaled to
  // the picture size. The rows below the panel (up to kRegionHeight) stay clear.
  Canvas stage(kStageWidth, kStageHeight);
  for (const Scene& scene : kScenes)
    if (scene.key == key) scene.draw(stage, value);
  Canvas panel(kPanelWidth, kPanelHeight);
  panel.RoundRect(0, 0, kPanelWidth, kPanelHeight, 14, kPanel);
  panel.Frame(0, 0, kPanelWidth, kPanelHeight, 3, Tint(kInk, 0.55f));
  panel.Composite(stage, 0, (kPanelHeight - kStageHeight) / 2);
  return panel.Scaled(kPictureSize, kPictureSize).ToRgba8(kTextureSize, kTextureSize);
}

}  // namespace mgr_pictures
