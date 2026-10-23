#include "mgr_button_atlas.h"

#include <rex/graphics/pipeline/texture/util.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t kBlockSize = 16;  // DXT5 bytes per 4x4 block
constexpr uint32_t kBlockBytesLog2 = 4;
constexpr uint32_t kBlocksX = kAtlasWidth / 4;
constexpr uint32_t kBlocksY = kAtlasHeight / 4;

// Exported from core.uvd (texture hash 0x79C096FA); act_btn_sign_02 stays as is.
constexpr IconRect kIcons[] = {
    {0, IconStyle::kAction, 0, 456, 49, 55},     {1, IconStyle::kAction, 68, 284, 49, 55},
    {2, IconStyle::kAction, 0, 400, 49, 55},     {3, IconStyle::kAction, 56, 344, 49, 55},
    {4, IconStyle::kAction, 0, 56, 82, 55},      {5, IconStyle::kAction, 0, 112, 82, 55},
    {6, IconStyle::kAction, 0, 168, 82, 55},     {7, IconStyle::kAction, 0, 0, 82, 55},
    {15, IconStyle::kAction, 0, 224, 65, 58},    {17, IconStyle::kAction, 0, 284, 65, 58},
    {0, IconStyle::kFilled, 84, 832, 29, 30},    {1, IconStyle::kFilled, 52, 832, 29, 30},
    {2, IconStyle::kFilled, 92, 800, 29, 30},    {3, IconStyle::kFilled, 0, 836, 29, 30},
    {4, IconStyle::kFilled, 0, 512, 45, 30},     {5, IconStyle::kFilled, 0, 576, 41, 30},
    {6, IconStyle::kFilled, 52, 512, 45, 30},    {7, IconStyle::kFilled, 52, 576, 41, 30},
    {8, IconStyle::kFilled, 92, 744, 33, 54},    {9, IconStyle::kFilled, 52, 456, 47, 54},
    {10, IconStyle::kFilled, 68, 224, 53, 54},   {11, IconStyle::kFilled, 52, 960, 25, 54},
    {12, IconStyle::kFilled, 84, 952, 25, 54},   {13, IconStyle::kFilled, 84, 112, 43, 54},
    {14, IconStyle::kFilled, 84, 168, 43, 54},   {15, IconStyle::kFilled, 52, 708, 38, 30},
    {16, IconStyle::kFilled, 0, 708, 38, 30},    {17, IconStyle::kFilled, 52, 676, 38, 30},
    {18, IconStyle::kFilled, 0, 676, 38, 30},    {19, IconStyle::kFilled, 0, 640, 39, 34},
    {20, IconStyle::kFilled, 92, 708, 34, 34},   {21, IconStyle::kFilled, 84, 864, 29, 30},
    {22, IconStyle::kFilled, 0, 900, 29, 30},    {23, IconStyle::kFilled, 52, 928, 29, 30},
    {24, IconStyle::kFilled, 52, 864, 29, 30},   {0, IconStyle::kOutline, 100, 544, 28, 29},
    {1, IconStyle::kOutline, 100, 512, 28, 29},  {2, IconStyle::kOutline, 100, 456, 28, 29},
    {3, IconStyle::kOutline, 100, 400, 28, 29},  {4, IconStyle::kOutline, 0, 544, 45, 29},
    {5, IconStyle::kOutline, 0, 608, 40, 29},    {6, IconStyle::kOutline, 52, 544, 45, 29},
    {7, IconStyle::kOutline, 52, 608, 40, 29},   {8, IconStyle::kOutline, 52, 776, 33, 54},
    {9, IconStyle::kOutline, 52, 400, 47, 54},   {10, IconStyle::kOutline, 0, 344, 53, 54},
    {11, IconStyle::kOutline, 0, 932, 25, 54},   {12, IconStyle::kOutline, 84, 896, 25, 54},
    {13, IconStyle::kOutline, 84, 0, 43, 54},    {14, IconStyle::kOutline, 84, 56, 43, 54},
    {15, IconStyle::kOutline, 92, 640, 36, 32},  {16, IconStyle::kOutline, 92, 676, 36, 32},
    {17, IconStyle::kOutline, 92, 608, 36, 32},  {18, IconStyle::kOutline, 0, 740, 36, 32},
    {19, IconStyle::kOutline, 52, 640, 39, 34},  {20, IconStyle::kOutline, 52, 740, 34, 34},
    {21, IconStyle::kOutline, 96, 576, 31, 30},  {22, IconStyle::kOutline, 0, 804, 31, 30},
    {23, IconStyle::kOutline, 0, 868, 29, 30},   {24, IconStyle::kOutline, 52, 896, 29, 30},
};

uint32_t BlockOffset(uint32_t bx, uint32_t by) {
  return uint32_t(rex::graphics::texture_util::GetTiledOffset2D(int32_t(bx), int32_t(by),
                                                                kBlocksX, kBlockBytesLog2));
}

// Guest data is 8-in-16 endian; host DXT5 layout is little endian.
void Swap16(const uint8_t* in, uint8_t* out) {
  for (uint32_t i = 0; i < kBlockSize; i += 2) {
    out[i] = in[i + 1];
    out[i + 1] = in[i];
  }
}

struct Rgb {
  int r, g, b;
};
Rgb Expand565(uint16_t v) {
  return {(v >> 11) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31};
}
uint16_t Pack565(int r, int g, int b) {
  return uint16_t((r * 31 + 127) / 255 << 11 | (g * 63 + 127) / 255 << 5 | (b * 31 + 127) / 255);
}

void AlphaPalette(uint8_t a0, uint8_t a1, uint8_t out[8]) {
  out[0] = a0;
  out[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) out[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) out[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    out[6] = 0;
    out[7] = 255;
  }
}

void ColorPalette(uint16_t c0, uint16_t c1, Rgb out[4]) {
  out[0] = Expand565(c0);
  out[1] = Expand565(c1);
  if (c0 > c1) {
    out[2] = {(2 * out[0].r + out[1].r) / 3, (2 * out[0].g + out[1].g) / 3,
              (2 * out[0].b + out[1].b) / 3};
    out[3] = {(out[0].r + 2 * out[1].r) / 3, (out[0].g + 2 * out[1].g) / 3,
              (out[0].b + 2 * out[1].b) / 3};
  } else {
    out[2] = {(out[0].r + out[1].r) / 2, (out[0].g + out[1].g) / 2, (out[0].b + out[1].b) / 2};
    out[3] = {0, 0, 0};
  }
}

// block: little-endian DXT5; px: 16 RGBA texels.
void DecodeBlock(const uint8_t* block, uint8_t px[64]) {
  uint8_t alpha[8];
  AlphaPalette(block[0], block[1], alpha);
  uint64_t alpha_bits = 0;
  for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
  Rgb color[4];
  ColorPalette(uint16_t(block[8] | block[9] << 8), uint16_t(block[10] | block[11] << 8), color);
  uint32_t color_bits;
  std::memcpy(&color_bits, block + 12, 4);
  for (int i = 0; i < 16; ++i) {
    const Rgb& c = color[(color_bits >> (2 * i)) & 3];
    px[i * 4 + 0] = uint8_t(c.r);
    px[i * 4 + 1] = uint8_t(c.g);
    px[i * 4 + 2] = uint8_t(c.b);
    px[i * 4 + 3] = alpha[(alpha_bits >> (3 * i)) & 7];
  }
}

template <typename T, size_t N, typename Distance>
uint32_t Nearest(const T (&palette)[N], Distance distance) {
  uint32_t best = 0;
  for (uint32_t i = 1; i < N; ++i) {
    if (distance(palette[i]) < distance(palette[best])) best = i;
  }
  return best;
}

// Bounding-box encoder; ample for the atlas's flat white glyphs.
void EncodeBlock(const uint8_t px[64], uint8_t* block) {
  uint8_t a_min = 255, a_max = 0;
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    const uint8_t a = px[i * 4 + 3];
    a_min = std::min(a_min, a);
    a_max = std::max(a_max, a);
    // Invisible texels constrain color too: additive HUD draws show it.
    for (int c = 0; c < 3; ++c) {
      lo[c] = std::min<int>(lo[c], px[i * 4 + c]);
      hi[c] = std::max<int>(hi[c], px[i * 4 + c]);
    }
  }

  uint8_t alpha[8];
  AlphaPalette(a_max, a_min, alpha);
  uint64_t alpha_bits = 0;
  uint16_t c0 = Pack565(hi[0], hi[1], hi[2]), c1 = Pack565(lo[0], lo[1], lo[2]);
  if (c0 < c1) std::swap(c0, c1);
  Rgb color[4];
  ColorPalette(c0, c1, color);
  uint32_t color_bits = 0;
  for (int i = 0; i < 16; ++i) {
    const int a = px[i * 4 + 3];
    const uint32_t ai =
        a_max == a_min ? 0 : Nearest(alpha, [&](uint8_t v) { return std::abs(int(v) - a); });
    alpha_bits |= uint64_t(ai) << (3 * i);
    const uint8_t* p = px + i * 4;
    // c0 == c1 selects 3-color mode, where index 0 is still the endpoint.
    const uint32_t ci = c0 == c1 ? 0 : Nearest(color, [&](const Rgb& c) {
      return (c.r - p[0]) * (c.r - p[0]) + (c.g - p[1]) * (c.g - p[1]) + (c.b - p[2]) * (c.b - p[2]);
    });
    color_bits |= ci << (2 * i);
  }
  block[0] = a_max;
  block[1] = a_min;
  for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t(alpha_bits >> (8 * i));
  block[8] = uint8_t(c0);
  block[9] = uint8_t(c0 >> 8);
  block[10] = uint8_t(c1);
  block[11] = uint8_t(c1 >> 8);
  std::memcpy(block + 12, &color_bits, 4);
}

void ReadBlockTexels(const Image& image, uint32_t bx, uint32_t by, uint8_t px[64]) {
  for (uint32_t i = 0; i < 16; ++i) {
    const size_t at = (size_t(by * 4 + i / 4) * image.width + bx * 4 + i % 4) * 4;
    std::memcpy(px + i * 4, image.rgba.data() + at, 4);
  }
}

void EncodeBlockAt(const Image& image, uint32_t bx, uint32_t by, uint8_t* tiled) {
  uint8_t px[64], block[kBlockSize];
  ReadBlockTexels(image, bx, by, px);
  EncodeBlock(px, block);
  Swap16(block, tiled + BlockOffset(bx, by));
}
}  // namespace

uint64_t Fnv1a64(std::span<const uint8_t> data) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (uint8_t byte : data) hash = (hash ^ byte) * 0x100000001b3ull;
  return hash;
}

std::span<const IconRect> AtlasIcons() { return kIcons; }

Image DecodeAtlas(std::span<const uint8_t> tiled) {
  Image image{kAtlasWidth, kAtlasHeight, std::vector<uint8_t>(kAtlasWidth * kAtlasHeight * 4)};
  if (tiled.size() < kAtlasBytes) return {};
  for (uint32_t by = 0; by < kBlocksY; ++by) {
    for (uint32_t bx = 0; bx < kBlocksX; ++bx) {
      uint8_t block[kBlockSize], px[64];
      Swap16(tiled.data() + BlockOffset(bx, by), block);
      DecodeBlock(block, px);
      for (uint32_t i = 0; i < 16; ++i) {
        const size_t at = (size_t(by * 4 + i / 4) * kAtlasWidth + bx * 4 + i % 4) * 4;
        std::memcpy(image.rgba.data() + at, px + i * 4, 4);
      }
    }
  }
  return image;
}

std::vector<uint8_t> EncodeAtlas(const Image& image) {
  std::vector<uint8_t> tiled(kAtlasBytes);
  for (uint32_t by = 0; by < kBlocksY; ++by) {
    for (uint32_t bx = 0; bx < kBlocksX; ++bx) EncodeBlockAt(image, bx, by, tiled.data());
  }
  return tiled;
}

std::vector<uint8_t> PatchAtlas(std::span<const uint8_t> original, const ButtonLabels& labels,
                                const IconRenderer& render, uint32_t round_buttons) {
  std::vector<uint8_t> patched(original.begin(), original.end());
  if (original.size() != kAtlasBytes) return patched;
  Image image = DecodeAtlas(original);
  std::vector<bool> dirty(kBlocksX * kBlocksY);
  for (const IconRect& rect : kIcons) {
    const std::string& label = labels[rect.button];
    if (label.empty()) continue;
    const Image icon = render(rect, label, (round_buttons >> rect.button) & 1);
    if (icon.width != rect.w || icon.height != rect.h || icon.rgba.size() != size_t(rect.w) * rect.h * 4) {
      continue;
    }
    for (uint32_t row = 0; row < rect.h; ++row) {
      std::memcpy(image.rgba.data() + (size_t(rect.y + row) * kAtlasWidth + rect.x) * 4,
                  icon.rgba.data() + size_t(row) * rect.w * 4, size_t(rect.w) * 4);
    }
    for (uint32_t by = rect.y / 4; by <= uint32_t(rect.y + rect.h - 1) / 4; ++by) {
      for (uint32_t bx = rect.x / 4; bx <= uint32_t(rect.x + rect.w - 1) / 4; ++bx) {
        dirty[by * kBlocksX + bx] = true;
      }
    }
  }
  for (uint32_t by = 0; by < kBlocksY; ++by) {
    for (uint32_t bx = 0; bx < kBlocksX; ++bx) {
      if (dirty[by * kBlocksX + bx]) EncodeBlockAt(image, bx, by, patched.data());
    }
  }
  return patched;
}
