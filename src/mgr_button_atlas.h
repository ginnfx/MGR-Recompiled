#pragma once

// MGR's controller button atlas: core.wtb texture 1 ("core_sys_btn"), 128x1024
// DXT5, Xenos-tiled, 8-in-16 endian (4B4E080A / 0.0.0.4). Pure format code with
// no guest-memory or renderer dependencies.

#include "mgr_key_labels.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

inline constexpr uint32_t kAtlasWidth = 128;
inline constexpr uint32_t kAtlasHeight = 1024;
inline constexpr size_t kAtlasBytes = kAtlasWidth * kAtlasHeight;  // DXT5: 1 byte/texel
// A unique 32-byte window of the original tiled data, for locating it in memory.
inline constexpr uint32_t kAtlasSignatureOffset = 0xC40;
inline constexpr std::array<uint8_t, 32> kAtlasSignature = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x51, 0x5a, 0xeb, 0xff, 0x00, 0x7e, 0x57,
    0xed, 0x69, 0xf3, 0xff, 0x92, 0xff, 0xdb, 0x60, 0x94, 0x72, 0x5a, 0xeb, 0xe0, 0xe0, 0x55, 0x7a};
// FNV-1a 64 of the full original tiled data.
inline constexpr uint64_t kAtlasHash = 0x7e895c4d2fe672c7;

uint64_t Fnv1a64(std::span<const uint8_t> data);

// core.uvd entries: btn_NNN (outline), btn_b_NNN (filled), act_btn_NNN (HUD).
enum class IconStyle : uint8_t { kOutline, kFilled, kAction };
struct IconRect {
  uint8_t button;  // index into ButtonLabels
  IconStyle style;
  uint16_t x, y, w, h;
};
std::span<const IconRect> AtlasIcons();

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // straight alpha, row-major
};

// Tiled guest bytes <-> RGBA.
Image DecodeAtlas(std::span<const uint8_t> tiled);
std::vector<uint8_t> EncodeAtlas(const Image& image);

// Draws one icon of rect.w x rect.h with the given label, on a round cap
// (PlayStation face buttons and sticks) or a keycap.
using IconRenderer =
    std::function<Image(const IconRect& rect, const std::string& label, bool round)>;

// Replaces every icon with a non-empty label; bit N of round_buttons selects a
// round cap for button N. Blocks no icon touches keep their original bytes.
std::vector<uint8_t> PatchAtlas(std::span<const uint8_t> original, const ButtonLabels& labels,
                                const IconRenderer& render, uint32_t round_buttons = 0);
