// Scaled viewport axes: integer scales must match the original
// GetHostViewportInfo arithmetic bit for bit; 3/2 must map guest coordinates
// onto host = guest * 1.5.
#include <rex/graphics/util/scaled_viewport.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>

using rex::graphics::ScaleRatio;
using rex::graphics::draw_util::HostViewportAxis;
using rex::graphics::draw_util::ScaleClippedViewportAxis;
using rex::graphics::draw_util::ScaleUnclippedViewportAxis;

namespace {

// Verbatim from draw.cpp before the ScaleRatio change (clip enabled branch).
HostViewportAxis OriginalClipped(uint32_t axis_resolution_scale, float offset_axis,
                                 float scale_axis, uint32_t axis_max_unscaled) {
  HostViewportAxis out;
  float scale_axis_abs = std::abs(scale_axis);
  float axis_max_unscaled_float = float(axis_max_unscaled);
  uint32_t axis_0_int =
      uint32_t(rex::clamp_float(offset_axis - scale_axis_abs, 0.0f, axis_max_unscaled_float));
  uint32_t axis_1_int =
      uint32_t(rex::clamp_float(offset_axis + scale_axis_abs, 0.0f, axis_max_unscaled_float));
  uint32_t axis_extent_int = axis_1_int - axis_0_int;
  out.offset = axis_0_int * axis_resolution_scale;
  out.extent = axis_extent_int * axis_resolution_scale;
  if (axis_extent_int) {
    float axis_extent_rounded = float(axis_extent_int);
    out.ndc_scale = scale_axis * 2.0f / axis_extent_rounded;
    out.ndc_offset = (float(offset_axis) - (float(axis_0_int) + axis_extent_rounded * 0.5f)) *
                     2.0f / axis_extent_rounded;
  } else {
    out.ndc_scale = 1.0f;
    out.ndc_offset = 0.0f;
  }
  return out;
}

// Verbatim from draw.cpp (clip disabled branch).
HostViewportAxis OriginalUnclipped(uint32_t scale, float scale_xy, float offset_base,
                                   float offset_add, uint32_t extent_axis_unscaled) {
  HostViewportAxis out;
  out.offset = 0;
  out.extent = extent_axis_unscaled * scale;
  float extent_axis_unscaled_float = float(extent_axis_unscaled);
  float pixels_to_ndc_axis = 2.0f / extent_axis_unscaled_float;
  out.ndc_scale = scale_xy * pixels_to_ndc_axis;
  out.ndc_offset = (offset_base - extent_axis_unscaled_float * 0.5f + offset_add) *
                   pixels_to_ndc_axis;
  return out;
}

bool Same(const HostViewportAxis& a, const HostViewportAxis& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// Host coordinate of guest NDC `ndc_in` through the guest viewport, then the
// host one: ((ndc_in * scale_axis + offset_axis) is the guest pixel).
double HostPixel(const HostViewportAxis& axis, double ndc_in) {
  const double ndc = ndc_in * axis.ndc_scale + axis.ndc_offset;
  return axis.offset + (ndc + 1.0) * 0.5 * axis.extent;
}

}  // namespace

int main() {
  std::mt19937 random(0x1080);
  std::uniform_real_distribution<float> offset(-64.0f, 1400.0f), scale(-800.0f, 800.0f);
  const float halves[] = {0.0f, 0.5f};
  for (uint32_t s = 1; s <= 3; ++s) {
    const ScaleRatio ratio = ScaleRatio::Integer(s);
    for (int i = 0; i < 200000; ++i) {
      const float o = offset(random) + halves[i & 1], sc = scale(random);
      const uint32_t max = 8192 / s;
      assert(Same(ScaleClippedViewportAxis(ratio, o, sc, max), OriginalClipped(s, o, sc, max)));
      assert(Same(ScaleUnclippedViewportAxis(ratio, sc, o, halves[i & 1], max),
                  OriginalUnclipped(s, sc, o, halves[i & 1], max)));
    }
    // Typical full-screen 720p viewport: offset 640/360, scale 640/-360.
    assert(Same(ScaleClippedViewportAxis(ratio, 640.5f, 640.0f, 8192),
                OriginalClipped(s, 640.5f, 640.0f, 8192)));
  }

  const ScaleRatio half3{3, 2};
  // Full-screen 1280x720 viewports land on 1920x1080 with guest edges mapped
  // exactly to host edges.
  for (const float half : halves) {
    const HostViewportAxis x = ScaleClippedViewportAxis(half3, 640.0f + half, 640.0f, 5461);
    assert(x.offset == 0 && x.extent == 1920);
    assert(std::abs(HostPixel(x, -1.0) - (half) * 1.5) < 1e-3);
    assert(std::abs(HostPixel(x, 1.0) - (1280.0 + half) * 1.5) < 1e-3);
    const HostViewportAxis y = ScaleClippedViewportAxis(half3, 360.0f + half, -360.0f, 5461);
    assert(y.offset == 0 && y.extent == 1080);
    assert(std::abs(HostPixel(y, 1.0) - half * 1.5) < 1e-3);
  }
  // Odd sub-viewports: host = guest * 1.5 for any guest coordinate.
  for (int i = 0; i < 100000; ++i) {
    const float o = std::abs(offset(random)) + 64.0f, sc = std::abs(scale(random)) * 0.25f + 1.0f;
    const HostViewportAxis a = ScaleClippedViewportAxis(half3, o, sc, 5461);
    if (!a.extent) continue;
    for (const double ndc_in : {-0.75, 0.0, 0.5}) {
      const double guest = o + sc * ndc_in;
      assert(std::abs(HostPixel(a, ndc_in) - guest * 1.5) < 2e-2);
    }
  }
  const HostViewportAxis unclipped = ScaleUnclippedViewportAxis(half3, 1.0f, 0.0f, 0.5f, 5460);
  assert(unclipped.extent == 8190);
  assert(std::abs(HostPixel(unclipped, 100.0) - 100.5 * 1.5) < 1e-3);

  std::cout << "scaled viewport: integer bit-identity and 3/2 mapping tests passed\n";
}
