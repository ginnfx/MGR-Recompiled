// ScaleRatio: integer ratios must reproduce the existing integer arithmetic
// exactly; 3/2 must tile guest ranges into host ranges without gaps.
#include <rex/graphics/util/scale_ratio.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using rex::graphics::ScaleRange;
using rex::graphics::ScaleRatio;

int main() {
  // Integer equivalence with `guest * s` and `host / s` (draw.cpp, caches).
  for (uint32_t s = 1; s <= 3; ++s) {
    const ScaleRatio r = ScaleRatio::Integer(s);
    assert(r.IsInteger() && ScaleRatio::IsSupported(r) && r.IsScaled() == (s != 1));
    for (uint32_t v = 0; v <= 16384; ++v) {
      assert(r.ScaleFloor(v) == v * s && r.ScaleCeil(v) == v * s);
      assert(r.UnscaleFloor(v) == v / s);
      assert(r.UnscaleCeil(v) == (v + s - 1) / s);
      assert(r.MapsExactly(v));
      const auto range = ScaleRange(r, v, v + 7);
      assert(range.begin == v * s && range.end == (v + 7) * s);
    }
  }

  const ScaleRatio half3 = {3, 2};
  assert(ScaleRatio::IsSupported(half3) && !half3.IsInteger() && half3.IsScaled());
  assert(!ScaleRatio::IsSupported({5, 2}) && !ScaleRatio::IsSupported({4, 1}));
  assert(half3 == (ScaleRatio{6, 4}) && !(half3 == ScaleRatio::Integer(2)));
  assert(half3.AsFloat() == 1.5f);
  // 1280x720 -> 1920x1080; EDRAM tiles 80x16 -> 120x24; resolve blocks 8 -> 12.
  assert(half3.ScaleFloor(1280) == 1920 && half3.ScaleFloor(720) == 1080);
  assert(half3.ScaleFloor(80) == 120 && half3.ScaleFloor(16) == 24 && half3.ScaleFloor(8) == 12);
  assert(half3.MapsExactly(80) && half3.MapsExactly(16) && !half3.MapsExactly(1));
  assert(half3.ScaleFloor(1) == 1 && half3.ScaleCeil(1) == 2);
  assert(half3.UnscaleFloor(1920) == 1280 && half3.UnscaleCeil(1919) == 1280);
  // 64-bit intermediates: no overflow near the 32-bit limit.
  assert(half3.ScaleFloor(0x80000000u) == 0xC0000000u);

  // Adjacent guest ranges tile the host axis with no gap or overlap, and a
  // single guest pixel always covers at least one host pixel (1x1 reductions).
  uint32_t expected_begin = 0;
  for (uint32_t v = 0; v < 4096; ++v) {
    const auto range = ScaleRange(half3, v, v + 1);
    assert(range.begin == expected_begin);
    assert(range.end - range.begin == 1 || range.end - range.begin == 2);
    expected_begin = range.end;
  }
  assert(expected_begin == 6144);
  const auto pixel0 = ScaleRange(half3, 0, 1);
  assert(pixel0.begin == 0 && pixel0.end == 1);
  const auto pixel1 = ScaleRange(half3, 1, 2);
  assert(pixel1.begin == 1 && pixel1.end == 3);
  // Even-aligned ranges map exactly.
  const auto even = ScaleRange(half3, 160, 320);
  assert(even.begin == 240 && even.end == 480);

  // Rectangles (scissors): integer scaling multiplies offset and extent, as the
  // Vulkan scissor code did; 3/2 maps both edges.
  using rex::graphics::ScaleRect;
  for (uint32_t s = 1; s <= 3; ++s) {
    const ScaleRatio r = ScaleRatio::Integer(s);
    for (uint32_t o = 0; o < 300; o += 7) {
      for (uint32_t e = 0; e < 300; e += 5) {
        const uint32_t offset[2] = {o, o + 1}, extent[2] = {e, e + 3};
        const auto rect = ScaleRect(r, r, offset, extent);
        assert(rect.offset[0] == o * s && rect.offset[1] == (o + 1) * s);
        assert(rect.extent[0] == e * s && rect.extent[1] == (e + 3) * s);
      }
    }
  }
  const uint32_t full_offset[2] = {0, 0}, full_extent[2] = {1280, 720};
  const auto full = ScaleRect(half3, half3, full_offset, full_extent);
  assert(full.offset[0] == 0 && full.extent[0] == 1920 && full.extent[1] == 1080);

  // ScaleExact / ScaleArea: integer scales equal the old multiplications.
  using rex::graphics::ScaleArea;
  for (uint32_t s = 1; s <= 3; ++s) {
    const ScaleRatio r = ScaleRatio::Integer(s);
    for (uint32_t v = 0; v <= 16384; ++v) assert(r.ScaleExact(v) == v * s);
    for (uint32_t t = 1; t <= 3; ++t)
      assert(ScaleArea(r, ScaleRatio::Integer(t), 10u << 20) == uint64_t(10u << 20) * s * t);
  }
  // EDRAM / render-target geometry at 3/2 (step 5): every tile and pitch
  // quantity the render-target cache scales is whole, for 32/64bpp and all
  // MSAA modes, so the asserting ScaleExact never fires on these inputs.
  for (uint32_t bpp64 = 0; bpp64 <= 1; ++bpp64) {
    for (uint32_t msaa_x_log2 = 0; msaa_x_log2 <= 1; ++msaa_x_log2) {
      const uint32_t tile_width = (80u >> bpp64) >> msaa_x_log2;  // 80/40/40/20
      assert(half3.MapsExactly(tile_width));
      for (uint32_t pitch_tiles = 1; pitch_tiles <= 16; ++pitch_tiles)
        assert(half3.MapsExactly(pitch_tiles * tile_width));
    }
  }
  for (uint32_t msaa_y_log2 = 0; msaa_y_log2 <= 1; ++msaa_y_log2) {
    const uint32_t tile_height = 16u >> msaa_y_log2;
    assert(half3.MapsExactly(tile_height));
    for (uint32_t rows = 1; rows <= 160; ++rows) assert(half3.MapsExactly(rows * tile_height));
  }
  assert(half3.ScaleExact(80) == 120 && half3.ScaleExact(16) == 24);
  assert(half3.ScaleExact(40) == 60 && half3.ScaleExact(20) == 30 && half3.ScaleExact(8) == 12);
  assert(half3.ScaleExact(8192) == 12288);
  // EDRAM buffer: 10 MiB x 9/4, one 120x24 tile = 2880 host samples.
  assert(ScaleArea(half3, half3, 10u << 20) == 23592960);
  assert(ScaleArea(half3, half3, 80 * 16) == 2880);
  // Resolve and clear rectangles are 8-pixel aligned: exact at 3/2.
  for (uint32_t v = 0; v <= 8192; v += 8) assert(half3.ScaleExact(v) == v / 2 * 3);

  // Settings and status text: the mgr_resolution_scale values, TOML floats and
  // the old integer values round-trip; anything unsupported is rejected.
  {
    using rex::graphics::FormatScaleRatio;
    using rex::graphics::ParseScaleRatio;
    struct Text {
      const char* text;
      ScaleRatio ratio;
    };
    const Text valid[] = {{"1", ScaleRatio::Integer(1)}, {"1.5", half3},
                          {"2", ScaleRatio::Integer(2)}, {"3", ScaleRatio::Integer(3)},
                          {"3/2", half3},                {"1.500000", half3},
                          {"2.000000", ScaleRatio::Integer(2)}};
    for (const Text& t : valid) {
      ScaleRatio parsed{};
      assert(ParseScaleRatio(t.text, parsed));
      assert(parsed.num == t.ratio.num && parsed.den == t.ratio.den);
    }
    for (const char* bad : {"", "0", "4", "1.25", "2x", "abc", "-1", "1.5 "}) {
      ScaleRatio parsed = ScaleRatio::Integer(2);
      assert(!ParseScaleRatio(bad, parsed));
    }
    assert(FormatScaleRatio(half3) == "1.5");
    for (uint32_t s = 1; s <= 3; ++s)
      assert(FormatScaleRatio(ScaleRatio::Integer(s)) == std::to_string(s));
    assert(ScaleRatio::Integer(3).RequireInteger("test") == 3);
  }

  std::cout << "scale ratio: integer equivalence, 3/2 mapping, range tiling, EDRAM geometry and "
               "settings text tests passed\n";
}
