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
#include "gpu/pipeline/pso_recorder.h"

#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(mgr_msaa, 0, "MGR/Graphics",
                     "Native renderer multisample count for scene render targets: 0, 2, 4 or 8");
REXCVAR_DEFINE_INT32(mgr_shadow_resolution, 0, "MGR/Graphics",
                     "Native renderer shadow map size in texels; 0 keeps the game's size.");
REXCVAR_DEFINE_BOOL(mgr_transparency_aa, true, "MGR/Graphics",
                    "Smooth the edges of alpha-tested surfaces (hair, fences, foliage) with "
                    "alpha to coverage. Needs MSAA");
REXCVAR_DEFINE_DOUBLE(mgr_sharpening, 0.0, "MGR/Graphics",
                      "Contrast adaptive sharpening of the final image, 0 (off) to 1");
REXCVAR_DEFINE_DOUBLE(mgr_brightness, 1.0, "MGR/Graphics",
                      "Image brightness, 0.5 to 1.5. 1 keeps the game's own output; higher "
                      "values lift dark and mid tones");
REXCVAR_DEFINE_STRING(mgr_upscaler, "off", "MGR/Graphics",
                      "Temporal upscaling of the 3D scene to the internal resolution: off, "
                      "metalfx_quality, metalfx_balanced or metalfx_performance. Turns MSAA off");

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

bool Settings::TransparencyAA() const { return REXCVAR_GET(mgr_transparency_aa); }

f32 Settings::UpscalerInputFraction() const {
  // Input size per axis as a fraction of the output (MetalFX's quality modes).
  const std::string mode = Flag("mgr_upscaler");
  if (mode == "metalfx_quality")
    return 1.0f / 1.5f;
  if (mode == "metalfx_balanced")
    return 1.0f / 1.7f;
  if (mode == "metalfx_performance")
    return 0.5f;
  return 1.0f;
}

f32 Settings::Brightness() const {
  constexpr f64 kMinBrightness = 0.5, kMaxBrightness = 1.5;
  return f32(std::clamp(REXCVAR_GET(mgr_brightness), kMinBrightness, kMaxBrightness));
}

f32 Settings::Sharpness() const {
  return f32(std::clamp(REXCVAR_GET(mgr_sharpening), 0.0, 1.0));
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
f32 ReadShadowRenderScale() {
  const i32 dimension = Settings::Get().ShadowDimension();
  return dimension > i32(kShadowMapSize) ? f32(dimension) / f32(kShadowMapSize) : 1.0f;
}
// Render thread only: RefreshRenderTargetSettings at device init and each
// present (cvars are not loaded yet at static init).
f32 g_scene_scale = 1.0f;
f32 g_scene_input_scale = 1.0f;
f32 g_shadow_scale = 1.0f;
i32 g_msaa = 0;
bool g_transparency_aa = false;
u32 g_target_generation = 1;
f32 g_max_upscale = 1.0f;
} // namespace

f32 SceneRenderScale() { return g_scene_scale; }

f32 SceneInputScale() { return g_scene_input_scale; }

bool UpscalerActive() { return g_scene_input_scale != g_scene_scale; }

void SetMaxUpscaleRatio(f32 ratio) { g_max_upscale = std::max(1.0f, ratio); }

f32 ShadowRenderScale() { return g_shadow_scale; }

i32 ActiveMSAA() { return g_msaa; }

u32 RenderTargetGeneration() { return g_target_generation; }

void RefreshRenderTargetSettings() {
  const f32 scene = ReadSceneRenderScaleCvar(), shadow = ReadShadowRenderScale();
  // The upscaler needs one sample per pixel and a ratio the device supports;
  // without MetalFX (g_max_upscale 1) the fraction clamps back to 1.
  const f32 fraction =
      std::max(Settings::Get().UpscalerInputFraction(), 1.0f / g_max_upscale);
  const f32 input = fraction < 1.0f ? scene * fraction : scene;
  const i32 msaa = input != scene ? 0 : Settings::Get().MSAA();
  const bool transparencyAA = Settings::Get().TransparencyAA();
  if (scene == g_scene_scale && input == g_scene_input_scale && shadow == g_shadow_scale &&
      msaa == g_msaa && transparencyAA == g_transparency_aa)
    return;
  g_scene_scale = scene;
  g_scene_input_scale = input;
  g_shadow_scale = shadow;
  // Transparency AA only matters while MSAA is on (alpha-to-coverage), but a
  // toggle then changes the scene pipelines' state, so it needs the same pass.
  const bool aa_state_changed =
      msaa != g_msaa || (msaa != 0 && transparencyAA != g_transparency_aa);
  g_msaa = msaa;
  g_transparency_aa = transparencyAA;
  ++g_target_generation;
  if (aa_state_changed)
    OnMSAAChanged();
}

bool IsShadowShapedTarget(u32 width, u32 height) {
  return width == kShadowMapSize && (height == kShadowMapSize || height == 2 * kShadowMapSize);
}

bool IsSceneShapedTarget(u32 width, u32 height) {
  // Screen-proportioned targets follow the scene scale: the 1280x720
  // composite, the 1152x720 3D scene, and their halves. Shadow maps,
  // reduction targets and cubes keep their size.
  const auto shaped = [width, height](u32 w, u32 h) { return u64(width) * h == u64(height) * w; };
  return (shaped(kGameWidth, kGameHeight) || shaped(kSceneWidth, kGameHeight)) &&
         width >= kMinScaledTargetWidth;
}

f32 SceneTargetScale(u32 width, u32 height, bool depth, bool surface) {
  // While upscaling, every render target the game binds renders the 3D scene
  // at the input scale (the composite is redirected to an output-size target,
  // see upscale.h). Colour textures of the 1280x720 shape take the output
  // scale: they hold what the composite and HUD resolve, the front buffer
  // among them.
  if (surface || depth || !IsUpscaledShape(width, height))
    return g_scene_input_scale;
  return g_scene_scale;
}

bool IsUpscaledShape(u32 width, u32 height) {
  return u64(width) * kGameHeight == u64(height) * kGameWidth;
}

u32 ScaleExtent(u32 extent, f32 scale) {
  return std::max(1u, u32(f32(extent) * scale + 0.5f));
}

} // namespace mgrn::gpu
