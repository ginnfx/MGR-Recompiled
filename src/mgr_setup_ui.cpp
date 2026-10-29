#include "mgr_setup_ui.h"

#include <ImageIO/ImageIO.h>
#include <rex/filesystem.h>
#include <rex/ui/immediate_drawer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace mgr_setup_ui {
namespace {

namespace fs = std::filesystem;

constexpr float kPad = 22.0f;
constexpr float kMaxPanelWidth = 620.0f;
constexpr float kCut = 22.0f;           // chamfer on two panel corners
constexpr float kOptionHeight = 40.0f;
constexpr float kFooterHeight = 46.0f;
constexpr float kArtLeft = 0.14f;       // key art starts 14% in, behind the panel
constexpr ImWchar kGlyphRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0};
// The two display sizes only ever draw these, which keeps the atlas (uploaded
// in one piece) inside the renderer's 16 MB upload chunk.
constexpr ImWchar kTitleGlyphs[] = {'.', '.', 'G', 'G', 'M', 'M', 'R', 'R', 0};
constexpr ImWchar kPercentGlyphs[] = {'%', '%', '0', '9', 0};
constexpr int kMaxBackdropWidth = 1920;

Fonts g_fonts;
float g_scale = 1.0f;

ImU32 WithAlpha(ImU32 color, float alpha) {
  return (color & ~IM_COL32_A_MASK) | (ImU32(std::clamp(alpha, 0.0f, 1.0f) * 255.0f) << IM_COL32_A_SHIFT);
}

ImFont* AddFont(ImFontAtlas* atlas, const char* file, float size,
              const ImWchar* glyphs = kGlyphRanges) {
  const fs::path path = AssetDir() / "fonts" / file;
  if (!fs::exists(path)) return nullptr;
  ImFontConfig cfg;
  cfg.RasterizerDensity = 2.0f;  // sharp on Retina; layout stays in points
  cfg.GlyphRanges = glyphs;
  return atlas->AddFontFromFileTTF(path.c_str(), size, &cfg);
}

// Decodes an image file to RGBA8 with ImageIO.
std::vector<uint8_t> LoadRgba(const fs::path& path, int& width, int& height) {
  std::vector<uint8_t> pixels;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.native().size()), false);
  if (!url) return pixels;
  CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (!source) return pixels;
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!image) return pixels;
  width = int(CGImageGetWidth(image));
  height = int(CGImageGetHeight(image));
  if (width > kMaxBackdropWidth) {  // one upload, and more than enough behind a panel
    height = int(int64_t(height) * kMaxBackdropWidth / width);
    width = kMaxBackdropWidth;
  }
  pixels.resize(size_t(width) * size_t(height) * 4);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(pixels.data(), size_t(width), size_t(height), 8,
                                           size_t(width) * 4, space,
                                           uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(space);
  if (ctx) {
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
    CGContextRelease(ctx);
  } else {
    pixels.clear();
  }
  CGImageRelease(image);
  return pixels;
}

void DrawTick(ImDrawList* dl, ImVec2 p, int state) {
  if (state == 1) {
    dl->AddLine({p.x, p.y + 6}, {p.x + 4, p.y + 10}, kIce, 1.6f);
    dl->AddLine({p.x + 4, p.y + 10}, {p.x + 11, p.y + 2}, kIce, 1.6f);
  } else if (state == 2) {
    dl->AddLine({p.x + 1, p.y + 1}, {p.x + 10, p.y + 10}, kRedHot, 1.6f);
    dl->AddLine({p.x + 10, p.y + 1}, {p.x + 1, p.y + 10}, kRedHot, 1.6f);
  } else {
    dl->AddCircleFilled({p.x + 3, p.y + 6}, 1.2f, kAsh);
    dl->AddCircleFilled({p.x + 8, p.y + 6}, 1.2f, kAsh);
  }
}

void KeyCap(ImDrawList* dl, ImVec2& cursor, std::string_view key, ImU32 color, bool round) {
  const auto& f = GetFonts();
  ImFont* font = f.mono_small ? f.mono_small : ImGui::GetFont();
  const float text_w = font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0, key.data(),
                                           key.data() + key.size()).x;
  const float w = round ? 22.0f : std::max(22.0f, text_w + 12.0f);
  const ImVec2 a{cursor.x, cursor.y - 16}, b{cursor.x + w, cursor.y + 6};
  if (round)
    dl->AddCircle({(a.x + b.x) / 2, (a.y + b.y) / 2}, 11.0f, color, 0, 1.2f);
  else
    dl->AddRect(a, b, color, 0, 0, 1.0f);
  Text(dl, font, {a.x + (w - text_w) / 2, a.y + 5}, round ? color : kBone, key);
  cursor.x += w + 7;
}

}  // namespace

const Fonts& GetFonts() { return g_fonts; }

void SetScale(float scale) { g_scale = scale; }

bool MouseIn(ImVec2 a, ImVec2 b) {
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float x = m.x / g_scale, y = m.y / g_scale;
  return x >= a.x && x < b.x && y >= a.y && y < b.y;
}

fs::path AssetDir() {
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  const fs::path beside = exe_dir / "setup";
  if (fs::exists(beside)) return beside;
  return exe_dir.parent_path() / "Resources" / "setup";
}

void LoadFonts(ImFontAtlas* atlas) {
  g_fonts.title = AddFont(atlas, "JosefinSans.ttf", 110.0f, kTitleGlyphs);
  g_fonts.big = AddFont(atlas, "JosefinSans.ttf", 84.0f, kPercentGlyphs);
  g_fonts.heading = AddFont(atlas, "JosefinSans.ttf", 40.0f);
  g_fonts.mark = AddFont(atlas, "JosefinSans.ttf", 22.0f);
  g_fonts.body = AddFont(atlas, "IBMPlexSansCondensed-Regular.ttf", 17.0f);
  g_fonts.option = AddFont(atlas, "IBMPlexSansCondensed-Medium.ttf", 17.0f);
  g_fonts.small = AddFont(atlas, "IBMPlexSansCondensed-Regular.ttf", 13.0f);
  g_fonts.mono = AddFont(atlas, "IBMPlexMono-Regular.ttf", 13.0f);
  g_fonts.mono_small = AddFont(atlas, "IBMPlexMono-Regular.ttf", 11.0f);
}

void Backdrop::Draw(ImDrawList* dl, ImVec2 size, rex::ui::ImmediateDrawer* drawer) {
  dl->AddRectFilled({0, 0}, size, kStorm);
  if (!tried_ && drawer) {
    tried_ = true;
    int w = 0, h = 0;
    const auto pixels = LoadRgba(AssetDir() / "background.jpg", w, h);
    if (!pixels.empty()) {
      texture_ = drawer->CreateTexture(uint32_t(w), uint32_t(h),
                                       rex::ui::ImmediateTextureFilter::kLinear, false,
                                       pixels.data());
      width_ = w;
      height_ = h;
      if (!texture_) tried_ = false;  // device not ready yet; retry next frame
    }
  }
  if (texture_) {
    // Cover the area right of kArtLeft, cropping around the subject.
    const ImVec2 a{size.x * kArtLeft, 0}, b = size;
    const float area_aspect = (b.x - a.x) / b.y;
    const float image_aspect = float(width_) / float(height_);
    ImVec2 uv0{0, 0}, uv1{1, 1};
    if (image_aspect > area_aspect) {
      const float span = area_aspect / image_aspect;
      uv0.x = (1 - span) * 0.55f;
      uv1.x = uv0.x + span;
    } else {
      const float span = image_aspect / area_aspect;
      uv0.y = (1 - span) * 0.22f;
      uv1.y = uv0.y + span;
    }
    dl->AddImage(reinterpret_cast<ImTextureID>(texture_.get()), a, b, uv0, uv1,
                 IM_COL32(230, 230, 230, 255));
  }
  // Darken toward the panel and the bottom edge, then scanlines.
  const float x30 = size.x * 0.30f, x58 = size.x * 0.58f;
  dl->AddRectFilledMultiColor({0, 0}, {x30, size.y}, WithAlpha(kVoid, .94f), WithAlpha(kVoid, .72f),
                              WithAlpha(kVoid, .72f), WithAlpha(kVoid, .94f));
  dl->AddRectFilledMultiColor({x30, 0}, {x58, size.y}, WithAlpha(kVoid, .72f), WithAlpha(kVoid, 0),
                              WithAlpha(kVoid, 0), WithAlpha(kVoid, .72f));
  dl->AddRectFilledMultiColor({0, size.y * 0.7f}, size, WithAlpha(kVoid, 0), WithAlpha(kVoid, 0),
                              WithAlpha(kVoid, .6f), WithAlpha(kVoid, .6f));
  for (float y = 0; y < size.y; y += 3.0f)
    dl->AddLine({0, y}, {size.x, y}, IM_COL32(0, 0, 0, 40));
}

void Text(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, std::string_view text) {
  if (!font) font = ImGui::GetFont();
  dl->AddText(font, font->LegacySize, pos, color, text.data(), text.data() + text.size());
}

float SpacedWidth(ImFont* font, std::string_view text, float spacing, float size) {
  if (!font) font = ImGui::GetFont();
  if (size <= 0) size = font->LegacySize;
  const float w = font->CalcTextSizeA(size, FLT_MAX, 0, text.data(), text.data() + text.size()).x;
  return w + spacing * float(text.size());
}

void SpacedText(ImDrawList* dl, ImFont* font, ImVec2 pos, ImU32 color, std::string_view text,
                float spacing, float size) {
  if (!font) font = ImGui::GetFont();
  if (size <= 0) size = font->LegacySize;
  for (size_t i = 0; i < text.size();) {
    // Step one UTF-8 code point at a time.
    size_t n = 1;
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    n = std::min(n, text.size() - i);
    const char* s = text.data() + i;
    dl->AddText(font, size, pos, color, s, s + n);
    pos.x += font->CalcTextSizeA(size, FLT_MAX, 0, s, s + n).x + spacing;
    i += n;
  }
}

void Panel::Begin(ImDrawList* dl, float x, float y, float width, bool measure) {
  dl_ = dl;
  x_ = x;
  y_ = y;
  width_ = width;
  measure_ = measure;
  option_count_ = 0;
}

std::string Panel::Draw(ImDrawList* dl, ImVec2 screen, int step,
                        const std::vector<std::string>& steps, const Content& content, int& focus,
                        bool confirm, bool mouse_enabled) {
  const auto& f = GetFonts();
  const float width = std::min(kMaxPanelWidth, screen.x - 32.0f);
  const float x = std::max(16.0f, screen.x * 0.05f);
  const float inner = width - 2 * kPad;
  const float header = 18 + 22 + 16 + 26;  // mark + rail

  focus_ = &focus;
  confirm_ = false;
  mouse_enabled_ = false;
  activated_.clear();
  Begin(dl, x + kPad, 0, inner, true);
  content(*this);
  const float body_height = y_;
  const float height = std::min(screen.y - 32.0f, header + 20 + body_height + 12 + kFooterHeight);
  const float top = std::max(16.0f, (screen.y - height) / 2);
  if (option_count_ > 0) focus = std::clamp(focus, 0, option_count_ - 1);

  // Panel body with two chamfered corners, frame and the red accent.
  const ImVec2 a{x, top}, b{x + width, top + height};
  const ImVec2 poly[] = {{a.x, a.y}, {b.x - kCut, a.y}, {b.x, a.y + kCut},
                         {b.x, b.y}, {a.x + kCut, b.y}, {a.x, b.y - kCut}};
  dl->AddConvexPolyFilled(poly, 6, kPanel);
  dl->AddPolyline(poly, 6, kLine, ImDrawFlags_Closed, 1.0f);
  dl->AddRectFilled(a, {a.x + 64, a.y + 2}, kRed);

  // Wordmark and step rail.
  float y = top + 18;
  SpacedText(dl, f.mark, {x + kPad, y}, kRedHot, "MGR.R.", 6.0f);
  SpacedText(dl, f.mono_small, {x + kPad + SpacedWidth(f.mark, "MGR.R.", 6.0f) + 12, y + 8}, kAsh,
             "RECOMPILED · MACOS", 2.8f);
  y += 22 + 16;
  const float seg = (inner - 4.0f * float(steps.size() - 1)) / float(steps.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    const float sx = x + kPad + float(i) * (seg + 4);
    const bool now = int(i) == step, done = int(i) < step;
    dl->AddRectFilled({sx, y}, {sx + seg, y + 2}, now ? kRed : done ? kAsh : kLine);
    char label[48];
    std::snprintf(label, sizeof(label), "%02zu %s", i + 1, steps[i].c_str());
    std::string upper = label;
    for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    dl->PushClipRect({sx, y}, {sx + seg, y + 24}, true);
    SpacedText(dl, f.mono_small, {sx, y + 8}, now ? kBone : kAsh, upper, 1.5f);
    dl->PopClipRect();
  }
  y += 26 + 20;

  // Content, then the footer prompts.
  focus_ = &focus;
  confirm_ = confirm;
  mouse_enabled_ = mouse_enabled;
  dl->PushClipRect({x, y - 4}, {x + width, b.y - kFooterHeight}, true);
  Begin(dl, x + kPad, y, inner, false);
  content(*this);
  dl->PopClipRect();
  dl->AddLine({x, b.y - kFooterHeight}, {b.x, b.y - kFooterHeight}, kLine);
  footer_anchor_ = {b.x - kPad, b.y - kFooterHeight / 2 + 6};
  focus_ = nullptr;
  return activated_;
}

void Panel::Eyebrow(std::string_view text) {
  if (!measure_) SpacedText(dl_, GetFonts().mono_small, {x_, y_}, kRedHot, text, 2.6f);
  y_ += 22;
}

void Panel::Heading(std::string_view text) {
  ImFont* font = GetFonts().heading ? GetFonts().heading : ImGui::GetFont();
  std::string upper(text);
  for (auto& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
  constexpr float kSpacing = 2.4f;
  // Shrink to fit rather than wrap: headings are one line, like the prototype.
  float size = font->LegacySize;
  const float natural = SpacedWidth(font, upper, kSpacing);
  if (natural > width_) size *= width_ / natural;
  if (!measure_) SpacedText(dl_, font, {x_, y_ + (font->LegacySize - size) * 0.5f}, kBone, upper, kSpacing, size);
  y_ += font->LegacySize + 14;
}

void Panel::Paragraph(std::string_view text, ImU32 color, ImFont* font) {
  if (!font) font = GetFonts().body ? GetFonts().body : ImGui::GetFont();
  const float wrap = std::min(width_, 560.0f);
  const float h = font->CalcTextSizeA(font->LegacySize, FLT_MAX, wrap, text.data(),
                                      text.data() + text.size()).y;
  if (!measure_)
    dl_->AddText(font, font->LegacySize, {x_, y_}, color, text.data(), text.data() + text.size(),
                 wrap);
  y_ += h + 12;
}

void Panel::Facts(const std::vector<Fact>& facts) {
  const auto& f = GetFonts();
  float label_w = 0;
  for (const auto& fact : facts) label_w = std::max(label_w, SpacedWidth(f.mono_small, fact.label, 2));
  y_ += 4;
  for (const auto& fact : facts) {
    ImFont* font = fact.mono ? f.mono : f.body;
    const float wrap = width_ - label_w - 18;
    const float h = std::max(20.0f, font->CalcTextSizeA(font->LegacySize, FLT_MAX, wrap,
                                                         fact.value.c_str()).y);
    if (!measure_) {
      SpacedText(dl_, f.mono_small, {x_, y_ + 4}, kAsh, fact.label, 2);
      dl_->AddText(font, font->LegacySize, {x_ + label_w + 18, y_ + (fact.mono ? 2 : 0)}, kBone,
                   fact.value.c_str(), nullptr, wrap);
    }
    y_ += h + 6;
  }
  y_ += 6;
}

void Panel::Checks(const std::vector<CheckRow>& rows) {
  const auto& f = GetFonts();
  for (const auto& row : rows) {
    if (!measure_) {
      const ImU32 dim = row.state == 0 ? 110 : 255;
      DrawTick(dl_, {x_, y_ + 6}, row.state);
      Text(dl_, f.body, {x_ + 26, y_ + 2}, WithAlpha(kBone, dim / 255.0f), row.label);
      const float vw = f.mono->CalcTextSizeA(f.mono->LegacySize, FLT_MAX, 0, row.value.c_str()).x;
      Text(dl_, f.mono, {x_ + width_ - vw, y_ + 4}, row.state == 2 ? kRedHot : kAsh, row.value);
      dl_->AddLine({x_, y_ + 31}, {x_ + width_, y_ + 31}, kLine);
    }
    y_ += 32;
  }
  y_ += 10;
}

void Panel::Alert(std::string_view text, ImU32 accent) {
  ImFont* font = GetFonts().body;
  const float wrap = width_ - 28;
  const float h = font->CalcTextSizeA(font->LegacySize, FLT_MAX, wrap, text.data(),
                                      text.data() + text.size()).y + 22;
  if (!measure_) {
    dl_->AddRectFilled({x_, y_}, {x_ + width_, y_ + h}, WithAlpha(accent, accent == kIce ? .07f : .12f));
    dl_->AddRectFilled({x_, y_}, {x_ + 2, y_ + h}, accent);
    dl_->AddText(font, font->LegacySize, {x_ + 14, y_ + 11}, kBone, text.data(),
                 text.data() + text.size(), wrap);
  }
  y_ += h + 12;
}

void Panel::Progress(float fraction, std::string_view big, std::string_view caption) {
  const auto& f = GetFonts();
  if (!measure_) {
    Text(dl_, f.big, {x_, y_}, kBone, big);
    const float bw = f.big->CalcTextSizeA(f.big->LegacySize, FLT_MAX, 0, big.data(),
                                          big.data() + big.size()).x;
    SpacedText(dl_, f.mono_small, {x_ + bw + 14, y_ + f.big->LegacySize * 0.62f}, kAsh, caption, 1.5f);
    const float by = y_ + f.big->LegacySize + 8;
    dl_->AddRectFilled({x_, by}, {x_ + width_, by + 10}, IM_COL32(235, 232, 227, 20));
    dl_->AddRect({x_, by}, {x_ + width_, by + 10}, kLine);
    const float fill = width_ * std::clamp(fraction, 0.0f, 1.0f);
    dl_->AddRectFilled({x_, by}, {x_ + fill, by + 10}, kRed);
    for (float sx = x_ + 10; sx < x_ + fill; sx += 12)
      dl_->AddRectFilled({sx, by}, {std::min(sx + 2, x_ + fill), by + 10}, kRedHot);
  }
  y_ += f.big->LegacySize + 8 + 10 + 14;
}

void Panel::Stats(const std::vector<Fact>& stats) {
  const auto& f = GetFonts();
  const float col = width_ / float(std::max<size_t>(1, stats.size()));
  if (!measure_) {
    for (size_t i = 0; i < stats.size(); ++i) {
      const float sx = x_ + col * float(i);
      SpacedText(dl_, f.mono_small, {sx, y_}, kAsh, stats[i].label, 2);
      Text(dl_, f.mono, {sx, y_ + 18}, kBone, stats[i].value);
    }
  }
  y_ += 44;
}

void Panel::Log(const std::vector<std::pair<std::string, bool>>& lines) {
  const auto& f = GetFonts();
  for (const auto& [text, current] : lines) {
    if (!measure_) {
      if (current)
        dl_->AddTriangleFilled({x_, y_ + 4}, {x_, y_ + 12}, {x_ + 6, y_ + 8}, kRedHot);
      else
        DrawTick(dl_, {x_ - 2, y_ + 1}, 1);
      Text(dl_, f.mono, {x_ + 18, y_}, current ? kBone : kAsh, text);
    }
    y_ += 19;
  }
  y_ += 8;
}

void Panel::Option(std::string id, std::string_view label, std::string_view hint, bool quiet) {
  const auto& f = GetFonts();
  const int index = option_count_++;
  if (index == 0) y_ += 6;
  if (!measure_) {
    const ImVec2 a{x_ - 8, y_}, b{x_ + width_ + 8, y_ + kOptionHeight};
    const bool hovered = mouse_enabled_ && MouseIn(a, b);
    if (hovered && ImGui::GetIO().MouseDelta.x * ImGui::GetIO().MouseDelta.x +
                           ImGui::GetIO().MouseDelta.y * ImGui::GetIO().MouseDelta.y > 0)
      *focus_ = index;
    const bool focused = *focus_ == index;
    if (focused) {
      dl_->AddRectFilledMultiColor(a, b, WithAlpha(kRed, .32f), WithAlpha(kRed, 0),
                                   WithAlpha(kRed, 0), WithAlpha(kRed, .32f));
      dl_->AddRectFilled(a, {a.x + 2, b.y}, kRed);
    }
    const float shift = focused ? 20.0f : 14.0f;
    const ImVec2 c{a.x + shift + 4, y_ + kOptionHeight / 2};
    const ImVec2 diamond[] = {{c.x, c.y - 4}, {c.x + 4, c.y}, {c.x, c.y + 4}, {c.x - 4, c.y}};
    if (focused)
      dl_->AddConvexPolyFilled(diamond, 4, kRed);
    else
      dl_->AddPolyline(diamond, 4, kAsh, ImDrawFlags_Closed, 1.0f);
    std::string upper(label);
    for (auto& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    SpacedText(dl_, f.option, {c.x + 16, y_ + 10}, quiet ? kAsh : kBone, upper, 0.7f);
    if (!hint.empty()) {
      const float hw = f.small->CalcTextSizeA(f.small->LegacySize, FLT_MAX, 0, hint.data(),
                                              hint.data() + hint.size()).x;
      Text(dl_, f.small, {b.x - 14 - hw, y_ + 13}, kAsh, hint);
    }
    if ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || (focused && confirm_))
      activated_ = std::move(id);
  }
  y_ += kOptionHeight + 6;
}

void Prompts(ImDrawList* dl, ImVec2 right_baseline, bool pad, bool with_back) {
  const auto& f = GetFonts();
  // Measure right to left by drawing into a scratch cursor first.
  struct Item { std::string_view key; std::string_view label; ImU32 color; bool round; };
  std::vector<Item> items = {{pad ? "+" : "↑↓", "MOVE", kBone, false},
                             {pad ? "A" : "ENTER", "SELECT", IM_COL32(89, 194, 106, 255), pad}};
  if (with_back) items.push_back({pad ? "B" : "ESC", "BACK", kRedHot, pad});
  float total = 0;
  for (const auto& item : items) {
    const float kw = item.round ? 22.0f
                                : std::max(22.0f, f.mono_small->CalcTextSizeA(
                                                      f.mono_small->LegacySize, FLT_MAX, 0,
                                                      item.key.data(), item.key.data() + item.key.size()).x + 12);
    total += kw + 7 + SpacedWidth(f.small, item.label, 1.0f) + 18;
  }
  ImVec2 cursor{right_baseline.x - total, right_baseline.y};
  for (const auto& item : items) {
    KeyCap(dl, cursor, item.key, item.round ? item.color : kAsh, item.round);
    SpacedText(dl, f.small, {cursor.x, cursor.y - 13}, kAsh, item.label, 1.0f);
    cursor.x += SpacedWidth(f.small, item.label, 1.0f) + 18;
  }
}

}  // namespace mgr_setup_ui
