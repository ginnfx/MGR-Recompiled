#include "mgr_picture_codec.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace mgr_pictures {
namespace {
constexpr uint32_t kBlockBytes = 16;
constexpr uint32_t kTileTexels = 128;  // 32 blocks of 4 texels

struct Rgb { int r, g, b; };

uint16_t Pack565(const Rgb& c) {
  return uint16_t(((c.r * 31 + 127) / 255) << 11 | ((c.g * 63 + 127) / 255) << 5 | ((c.b * 31 + 127) / 255));
}
Rgb Unpack565(uint16_t v) {
  const int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
  return {(r * 255 + 15) / 31, (g * 255 + 31) / 63, (b * 255 + 15) / 31};
}
int Distance(const Rgb& a, const Rgb& b) {
  return (a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b);
}

// Bounding box endpoints with nearest-palette indices: plenty for flat
// diagrams, where a block holds one or two colours.
void EncodeColor(const std::array<Rgb, 16>& px, uint8_t* out) {
  Rgb lo = px[0], hi = px[0];
  for (const Rgb& c : px) {
    lo = {std::min(lo.r, c.r), std::min(lo.g, c.g), std::min(lo.b, c.b)};
    hi = {std::max(hi.r, c.r), std::max(hi.g, c.g), std::max(hi.b, c.b)};
  }
  uint16_t c0 = Pack565(hi), c1 = Pack565(lo);
  if (c0 < c1) std::swap(c0, c1);
  const Rgb p0 = Unpack565(c0), p1 = Unpack565(c1);
  const std::array<Rgb, 4> palette = {p0, p1, Rgb{(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                                      Rgb{(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
  uint32_t bits = 0;
  if (c0 != c1) {
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 4; ++k)
        if (Distance(px[i], palette[k]) < Distance(px[i], palette[best])) best = k;
      bits |= uint32_t(best) << (2 * i);
    }
  }
  out[0] = uint8_t(c0); out[1] = uint8_t(c0 >> 8);
  out[2] = uint8_t(c1); out[3] = uint8_t(c1 >> 8);
  for (int i = 0; i < 4; ++i) out[4 + i] = uint8_t(bits >> (8 * i));
}

void EncodeAlpha(const std::array<int, 16>& alpha, uint8_t* out) {
  const int a0 = *std::max_element(alpha.begin(), alpha.end());
  const int a1 = *std::min_element(alpha.begin(), alpha.end());
  std::array<int, 8> palette{a0, a1};
  for (int i = 1; i <= 6; ++i) palette[1 + i] = ((7 - i) * a0 + i * a1) / 7;
  uint64_t bits = 0;
  if (a0 != a1) {
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k)
        if (std::abs(alpha[i] - palette[k]) < std::abs(alpha[i] - palette[best])) best = k;
      bits |= uint64_t(best) << (3 * i);
    }
  }
  out[0] = uint8_t(a0); out[1] = uint8_t(a1);
  for (int i = 0; i < 6; ++i) out[2 + i] = uint8_t(bits >> (8 * i));
}

void SwapBytePairs(uint8_t* block) {
  for (uint32_t i = 0; i < kBlockBytes; i += 2) std::swap(block[i], block[i + 1]);
}

void CheckDimensions(uint32_t width, uint32_t height) {
  if (!width || !height || width % kTileTexels || height % kTileTexels)
    throw std::invalid_argument("picture size must be a multiple of 128 texels");
}
}  // namespace

uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks) {
  const uint32_t pitch = (pitch_blocks + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << 11;
  const uint32_t micro = ((x & 7) + ((y & 14) << 2)) << 4;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 1) << 4);
  return ((off & ~511u) << 3) + ((y & 16) << 7) + ((off & 448) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 63);
}

size_t TiledDxt5Size(uint32_t width, uint32_t height) {
  return size_t(width) * height;  // DXT5 is one byte per texel
}

void EncodeTiledDxt5Region(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, Region region,
                           uint8_t* tiled) {
  CheckDimensions(width, height);
  if (rgba.size() != size_t(width) * height * 4) throw std::invalid_argument("rgba size does not match dimensions");
  if (region.x % 4 || region.y % 4 || region.width % 4 || region.height % 4 || region.x + region.width > width ||
      region.y + region.height > height)
    throw std::invalid_argument("region must be block aligned and inside the image");
  const uint32_t pitch = width / 4;
  for (uint32_t by = region.y / 4; by < (region.y + region.height) / 4; ++by) {
    for (uint32_t bx = region.x / 4; bx < (region.x + region.width) / 4; ++bx) {
      std::array<Rgb, 16> color;
      std::array<int, 16> alpha;
      for (int i = 0; i < 16; ++i) {
        const uint8_t* p = &rgba[((by * 4 + i / 4) * size_t(width) + bx * 4 + i % 4) * 4];
        color[i] = {p[0], p[1], p[2]};
        alpha[i] = p[3];
      }
      uint8_t* block = tiled + TiledBlockOffset(bx, by, pitch);
      EncodeAlpha(alpha, block);
      EncodeColor(color, block + 8);
      SwapBytePairs(block);
    }
  }
}

void CopyTiledRegion(const uint8_t* source, uint8_t* tiled, uint32_t width, uint32_t height, Region region) {
  CheckDimensions(width, height);
  if (region.x % 4 || region.y % 4 || region.width % 4 || region.height % 4 || region.x + region.width > width ||
      region.y + region.height > height)
    throw std::invalid_argument("region must be block aligned and inside the image");
  const uint32_t pitch = width / 4;
  for (uint32_t by = region.y / 4; by < (region.y + region.height) / 4; ++by) {
    for (uint32_t bx = region.x / 4; bx < (region.x + region.width) / 4; ++bx) {
      const uint32_t offset = TiledBlockOffset(bx, by, pitch);
      std::copy_n(source + offset, kBlockBytes, tiled + offset);
    }
  }
}

std::vector<uint8_t> EncodeTiledDxt5(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
  std::vector<uint8_t> out(TiledDxt5Size(width, height));
  EncodeTiledDxt5Region(rgba, width, height, {0, 0, width, height}, out.data());
  return out;
}

std::vector<uint8_t> DecodeTiledDxt5(const uint8_t* data, uint32_t width, uint32_t height) {
  CheckDimensions(width, height);
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  const uint32_t pitch = width / 4;
  for (uint32_t by = 0; by < height / 4; ++by) {
    for (uint32_t bx = 0; bx < pitch; ++bx) {
      std::array<uint8_t, kBlockBytes> block;
      std::copy_n(data + TiledBlockOffset(bx, by, pitch), kBlockBytes, block.begin());
      SwapBytePairs(block.data());
      std::array<int, 8> alpha{block[0], block[1]};
      for (int i = 1; i <= 6; ++i)
        alpha[1 + i] = block[0] > block[1] ? ((7 - i) * block[0] + i * block[1]) / 7 : 0;
      if (block[0] <= block[1]) {
        for (int i = 1; i <= 4; ++i) alpha[1 + i] = ((5 - i) * block[0] + i * block[1]) / 5;
        alpha[6] = 0; alpha[7] = 255;
      }
      uint64_t alpha_bits = 0;
      for (int i = 0; i < 6; ++i) alpha_bits |= uint64_t(block[2 + i]) << (8 * i);
      const Rgb p0 = Unpack565(uint16_t(block[8] | block[9] << 8)), p1 = Unpack565(uint16_t(block[10] | block[11] << 8));
      const std::array<Rgb, 4> palette = {p0, p1, Rgb{(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                                          Rgb{(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
      const uint32_t color_bits = block[12] | block[13] << 8 | block[14] << 16 | uint32_t(block[15]) << 24;
      for (int i = 0; i < 16; ++i) {
        uint8_t* p = &rgba[((by * 4 + i / 4) * size_t(width) + bx * 4 + i % 4) * 4];
        const Rgb& c = palette[(color_bits >> (2 * i)) & 3];
        p[0] = uint8_t(c.r); p[1] = uint8_t(c.g); p[2] = uint8_t(c.b);
        p[3] = uint8_t(alpha[(alpha_bits >> (3 * i)) & 7]);
      }
    }
  }
  return rgba;
}

}  // namespace mgr_pictures
