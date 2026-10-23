// Input scaling for the model LOD chooser sub_82BF8758 (mgr_lod, mgr_lod.cpp).
// The chooser compares an on-screen size metric (bounds / (tan(25 deg) *
// distance) * 100) with OBJ_LOD0-2 (60/30/15, lod.bxm) and culls models past
// OBJ_LOD3 (100) by camera distance. Scaling the two inputs moves every model's
// thresholds at once without touching the data.
#pragma once

#include <limits>
#include <string_view>

namespace mgr_lod {

enum class Mode { kGame, kHigh, kMax };

struct Inputs {
  float distance;  // camera distance, compared with the OBJ_LOD3 cull distance
  float metric;    // % of screen, compared with OBJ_LOD0-2
};

constexpr Mode Parse(std::string_view value) {
  if (value == "high") return Mode::kHigh;
  if (value == "max") return Mode::kMax;
  return Mode::kGame;
}

// detail multiplies the size metric; reach divides the distance.
struct Bias {
  float detail;
  float reach;
};

constexpr Bias BiasFor(Mode mode) {
  switch (mode) {
    case Mode::kHigh: return {2.0f, 2.0f};
    // An infinite metric is above every threshold: level 0 at any distance.
    case Mode::kMax: return {std::numeric_limits<float>::infinity(), 4.0f};
    case Mode::kGame: break;
  }
  return {1.0f, 1.0f};
}

constexpr Inputs Apply(Mode mode, Inputs in) {
  const Bias bias = BiasFor(mode);
  // 0 * infinity is NaN; a model without bounds keeps its metric of 0.
  const float metric = in.metric > 0.0f ? in.metric * bias.detail : in.metric;
  return {in.distance / bias.reach, metric};
}

}  // namespace mgr_lod
