/**
 * @file    gpu/settings.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR (see settings.h).
 */
#include <algorithm>
#include "gpu/settings.h"

#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(mgr_msaa, 0, "MGR/Graphics",
                     "Native renderer multisample count for scene render targets: 0, 2, 4 or 8. "
                     "Applies on the next launch.");
REXCVAR_DEFINE_INT32(mgr_shadow_resolution, 0, "MGR/Graphics",
                     "Native renderer shadow map size in texels; 0 keeps the game's size.");
REXCVAR_DEFINE_DOUBLE(mgr_brightness, 1.0, "MGR/Graphics",
                      "Image brightness, 0.5 to 1.5. 1 keeps the game's own output; higher "
                      "values lift dark and mid tones");

namespace mgrn::gpu {

namespace {

std::string Flag(const char *name) { return rex::cvar::GetFlagByName(name); }

} // namespace

Settings &Settings::Get() {
  static Settings settings;
  return settings;
}

i32 Settings::Anisotropy() const {
  const std::string value = Flag("mgr_texture_filtering");
  if (value == "off")
    return 1;
  if (value.size() >= 2 && value.back() == 'x')
    return std::stoi(value.substr(0, value.size() - 1));
  return 0; // "game"
}

bool Settings::Vsync() const { return Flag("mgr_vsync") != "false"; }

i32 Settings::AspectRatio() const {
  return static_cast<i32>(Flag("present_letterbox") == "false" ? AspectMode::Stretch
                                                               : AspectMode::Original);
}

i32 Settings::ShadowDimension() const { return REXCVAR_GET(mgr_shadow_resolution); }

i32 Settings::MSAA() const { return REXCVAR_GET(mgr_msaa); }

f32 Settings::Brightness() const {
  constexpr f64 kMinBrightness = 0.5, kMaxBrightness = 1.5;
  return f32(std::clamp(REXCVAR_GET(mgr_brightness), kMinBrightness, kMaxBrightness));
}

namespace {
f32 ReadSceneRenderScaleCvar() {
  try {
    const f32 scale = std::stof(Flag("mgr_resolution_scale"));
    return std::clamp(scale, kMinSceneRenderScale, kMaxSceneRenderScale);
  } catch (...) {
    return 1.0f;
  }
}
// Render thread only: RefreshSceneRenderScale at device init and each present
// (cvars are not loaded yet at static init).
f32 g_scene_scale = 1.0f;
u32 g_scene_scale_generation = 1;
} // namespace

f32 SceneRenderScale() { return g_scene_scale; }

u32 SceneScaleGeneration() { return g_scene_scale_generation; }

void RefreshSceneRenderScale() {
  const f32 scale = ReadSceneRenderScaleCvar();
  if (scale == g_scene_scale)
    return;
  g_scene_scale = scale;
  ++g_scene_scale_generation;
}

bool IsSceneShapedTarget(u32 width, u32 height) {
  // Screen-proportioned targets (1280x720 and its halves) follow the scene
  // scale; shadow maps, reduction targets and cubes keep their size.
  return u64(width) * kGameHeight == u64(height) * kGameWidth &&
         width >= kMinScaledTargetWidth;
}

u32 ScaleExtent(u32 extent, f32 scale) {
  return std::max(1u, u32(f32(extent) * scale + 0.5f));
}

} // namespace mgrn::gpu
