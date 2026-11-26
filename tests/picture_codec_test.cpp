// Host-only checks for the Xbox 360 DXT5 tiled encoder used by the Options pictures.
#include "mgr_options_pictures.h"
#include "mgr_picture_codec.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <set>
#include <vector>

using namespace mgr_pictures;

static void TilingMatchesReferenceOffsets() {
  // Values from tools/assets/prepare_ui_font.py tiled_offset(x, y, 128).
  struct Case { uint32_t x, y, offset; };
  const Case cases[] = {{0, 0, 0}, {1, 0, 32}, {0, 1, 16}, {7, 3, 1840}, {31, 31, 16240},
                        {32, 0, 16384}, {0, 32, 65536}, {127, 127, 262000}, {100, 45, 127632}, {8, 16, 2112}};
  for (const Case& c : cases) assert(TiledBlockOffset(c.x, c.y, 128) == c.offset);
}

static void TilingIsABijection() {
  std::set<uint32_t> seen;
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x) {
      const uint32_t offset = TiledBlockOffset(x, y, 128);
      assert(offset % 16 == 0 && offset < 128 * 128 * 16);
      assert(seen.insert(offset).second);
    }
}

static std::vector<uint8_t> Image(uint32_t w, uint32_t h, auto pixel) {
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) pixel(x, y, &rgba[(size_t(y) * w + x) * 4]);
  return rgba;
}

static void FlatBlocksRoundTripWithinQuantisation() {
  const auto image = Image(128, 128, [](uint32_t x, uint32_t y, uint8_t* p) {
    const bool left = x < 64, top = y < 64;
    p[0] = left ? 255 : 0; p[1] = top ? 255 : 0; p[2] = 128; p[3] = (x + y) % 64 < 32 ? 255 : 0;
  });
  const auto encoded = EncodeTiledDxt5(image, 128, 128);
  assert(encoded.size() == TiledDxt5Size(128, 128));
  const auto decoded = DecodeTiledDxt5(encoded.data(), 128, 128);
  for (size_t i = 0; i < image.size(); i += 4) {
    // Alpha is the one channel exact for two-level blocks; colour has 5/6 bit endpoints.
    assert(std::abs(int(image[i + 3]) - int(decoded[i + 3])) <= 1);
    assert(std::abs(int(image[i]) - int(decoded[i])) <= 8);
    assert(std::abs(int(image[i + 1]) - int(decoded[i + 1])) <= 4);
    assert(std::abs(int(image[i + 2]) - int(decoded[i + 2])) <= 8);
  }
}

static void EveryOptionPictureRendersAnOpaquePanel() {
  const char* keys[] = {"fullscreen", "mgr_output_resolution", "mgr_resolution_scale", "mgr_upscaler", "mgr_msaa",
                        "mgr_transparency_aa", "mgr_shadow_resolution", "present_letterbox", "mgr_vsync",
                        "mgr_frame_rate", "mgr_brightness", "mgr_sharpening", "mgr_texture_filtering", "mgr_lod",
                        "mgr_bloom", "mgr_film_grain", "mgr_prompt_style", "mnk_mouse", "mnk_sensitivity",
                        "mgr_skip_intro_logos", "unknown_row"};
  const char* values[] = {"true", "false", "1", "3", "16x", "off", "max", "0.6", "keyboard", "unlocked"};
  for (const char* key : keys)
    for (const char* value : values) {
      const auto rgba = mgr_pictures::RenderOptionPicture(key, value);
      assert(rgba.size() == size_t(mgr_pictures::kTextureSize) * mgr_pictures::kTextureSize * 4);
      const size_t centre = (size_t(mgr_pictures::kPictureSize / 2) * mgr_pictures::kTextureSize + mgr_pictures::kPictureSize / 2) * 4;
      assert(rgba[centre + 3] > 100);          // panel covers the picture area
      assert(rgba[(size_t(200) * 512 + mgr_pictures::kRegionWidth + 10) * 4 + 3] == 0);  // right of it stays clear
      assert(rgba[(size_t(mgr_pictures::kRegionHeight + 10) * 512 + 100) * 4 + 3] == 0);  // and below
    }
}

static void CopyTiledRegionRestoresOnlyItsBlocks() {
  const std::vector<uint8_t> original(512 * 512, 0x11);
  std::vector<uint8_t> work(512 * 512, 0x22);
  const mgr_pictures::Region region{0, 0, 128, 64};
  mgr_pictures::CopyTiledRegion(original.data(), work.data(), 512, 512, region);
  size_t restored = 0;
  for (uint8_t byte : work) restored += byte == 0x11;
  assert(restored == size_t(region.width) * region.height);  // DXT5: one byte per texel
  bool threw = false;
  try { mgr_pictures::CopyTiledRegion(original.data(), work.data(), 512, 512, {2, 0, 8, 8}); } catch (const std::exception&) { threw = true; }
  assert(threw);
}

static void RejectsBadDimensions() {
  bool threw = false;
  try { EncodeTiledDxt5(std::vector<uint8_t>(100 * 100 * 4), 100, 100); } catch (const std::exception&) { threw = true; }
  assert(threw);
}

// MGR_PICTURE_DUMP=<dir>: write each picture (after the encode/decode round trip) as a PPM,
// composited over mid grey, to judge the artwork.
static void DumpPictures(const char* dir) {
  const struct { const char* key; const char* value; } views[] = {
      {"fullscreen", "true"}, {"fullscreen", "false"}, {"mgr_output_resolution", "display"},
      {"mgr_resolution_scale", "1"}, {"mgr_resolution_scale", "3"}, {"mgr_upscaler", "metalfx_quality"},
      {"mgr_msaa", "0"}, {"mgr_msaa", "4"}, {"mgr_transparency_aa", "true"}, {"mgr_shadow_resolution", "0"},
      {"present_letterbox", "false"}, {"mgr_vsync", "false"}, {"mgr_frame_rate", "120"}, {"mgr_brightness", "1.2"},
      {"mgr_sharpening", "0.6"}, {"mgr_texture_filtering", "off"}, {"mgr_texture_filtering", "16x"},
      {"mgr_lod", "game"}, {"mgr_bloom", "true"}, {"mgr_film_grain", "true"}, {"mgr_prompt_style", "auto"},
      {"mgr_prompt_style", "playstation"}, {"mnk_mouse", "true"}, {"mnk_sensitivity", "2"}, {"mgr_skip_intro_logos", "true"}};
  for (const auto& v : views) {
    const auto rgba = mgr_pictures::RenderOptionPicture(v.key, v.value);
    const auto decoded = DecodeTiledDxt5(EncodeTiledDxt5(rgba, 512, 512).data(), 512, 512);
    const std::string path = std::string(dir) + "/" + v.key + "-" + v.value + ".ppm";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) continue;
    std::fprintf(f, "P6 420 420 255\n");
    for (int y = 0; y < 420; ++y)
      for (int x = 0; x < 420; ++x) {
        const uint8_t* p = &decoded[(size_t(y) * 512 + x) * 4];
        for (int k = 0; k < 3; ++k) std::fputc((p[k] * p[3] + 90 * (255 - p[3])) / 255, f);
      }
    std::fclose(f);
  }
}

int main() {
  if (const char* dir = std::getenv("MGR_PICTURE_DUMP")) { DumpPictures(dir); return 0; }
  TilingMatchesReferenceOffsets();
  TilingIsABijection();
  FlatBlocksRoundTripWithinQuantisation();
  CopyTiledRegionRestoresOnlyItsBlocks();
  RejectsBadDimensions();
  EveryOptionPictureRendersAnOpaquePanel();
  std::puts("picture codec checks passed");
  return 0;
}
