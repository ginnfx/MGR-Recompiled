#include "mgr_lod_bias.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
  // Unknown values fall back to the game's own LOD.
  assert(mgr_lod::Parse("game") == mgr_lod::Mode::kGame);
  assert(mgr_lod::Parse("high") == mgr_lod::Mode::kHigh);
  assert(mgr_lod::Parse("max") == mgr_lod::Mode::kMax);
  assert(mgr_lod::Parse("") == mgr_lod::Mode::kGame);
  assert(mgr_lod::Parse("ultra") == mgr_lod::Mode::kGame);

  // game: the chooser sees its inputs unchanged.
  const auto game = mgr_lod::Apply(mgr_lod::Mode::kGame, {50.0f, 20.0f});
  assert(game.distance == 50.0f && game.metric == 20.0f);

  // high: the model looks twice as large and half as far away. A 20% model
  // (game level 1 against OBJ_LOD 60/30/15) reaches level 0 at 40%, and the
  // OBJ_LOD3 cull (100) moves out to 200.
  const auto high = mgr_lod::Apply(mgr_lod::Mode::kHigh, {150.0f, 20.0f});
  assert(high.distance == 75.0f && high.metric == 40.0f);

  // max: any visible model is above every threshold (level 0); the cull
  // distance moves out four times.
  const auto max = mgr_lod::Apply(mgr_lod::Mode::kMax, {300.0f, 0.01f});
  assert(max.distance == 75.0f && std::isinf(max.metric) && max.metric > 0);
  // A degenerate model with no bounds keeps metric 0 rather than NaN.
  const auto empty = mgr_lod::Apply(mgr_lod::Mode::kMax, {10.0f, 0.0f});
  assert(empty.metric == 0.0f && !std::isnan(empty.metric));

  std::cout << "lod_bias: all checks passed\n";
  return 0;
}
