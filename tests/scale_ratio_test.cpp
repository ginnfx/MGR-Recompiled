// ScaleRatio: integer ratios must reproduce the existing integer arithmetic
// exactly; 3/2 must tile guest ranges into host ranges without gaps.
#include <rex/graphics/util/scale_ratio.h>

#include <cassert>
#include <cstdint>
#include <iostream>

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

  std::cout << "scale ratio: integer equivalence, 3/2 mapping and range tiling tests passed\n";
}
