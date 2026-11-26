// Original, text-free diagrams that illustrate each Options row. They are drawn at
// run time (no game art is used or shipped) and swapped into the retail UI sprite's
// texture by mgr_native_options.cpp.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mgr_pictures {

// The retail sprite's texture is 512x512 and the sprite shows its top-left corner
// 1:1, from screen (700, 118) (traced 2026-11-26 with a grid). The controller drawing
// fills x 35..385, y 5..400; two small atlas pieces for the cursor arrows sit at
// (432..464, 8..14) and (5..20, 460..475) and must not be touched. The region we own
// is (0, 0, kRegionWidth, kRegionHeight); the picture itself is kPictureSize square.
constexpr uint32_t kTextureSize = 512;
constexpr int kRegionWidth = 420;
constexpr int kRegionHeight = 448;
constexpr int kPictureSize = 420;

// RGBA (straight alpha) kTextureSize^2 image for the row `key` set to `value`
// (the cvar's current value text). Unknown rows get a neutral panel.
std::vector<uint8_t> RenderOptionPicture(std::string_view key, std::string_view value);

}  // namespace mgr_pictures
