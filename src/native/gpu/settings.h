/**
 * @file    gpu/settings.h
 * @brief   Renderer settings for the MGR native renderer.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR: re:Blue's settings model is replaced by reads of the MGR
 * cvars (src/mgr_settings.cpp) and the native-only ones defined in
 * settings.cpp. Accessor names stay as the ported renderer calls them.
 */
#pragma once

#include <rex/types.h>

namespace mgrn::gpu {

// How the finished frame fits the window. MGR offers letterboxed (Original)
// or stretched output; the wider modes are kept for the ported output code.
enum class AspectMode : i32 {
  Original = 0, // 16:9
  Standard = 1, // 4:3
  Wide = 2,     // 16:10
  Ultrawide = 3,
  SuperUltrawide = 4,
  Auto = 5,
  Stretch = 6,
};

class Settings {
public:
  static Settings &Get();

  // mgr_texture_filtering: 0 keeps the game's sampler anisotropy.
  i32 Anisotropy() const;
  // mgr_vsync.
  bool Vsync() const;
  // present_letterbox: letterbox (Original) or Stretch.
  i32 AspectRatio() const;
  // mgr_shadow_resolution: 0 keeps the game's shadow map size.
  i32 ShadowDimension() const;
  // mgr_msaa: 0/2/4/8; targets follow it from the next frame.
  i32 MSAA() const;
  // mgr_transparency_aa: alpha-tested draws into MSAA targets use coverage.
  bool TransparencyAA() const;
  // mgr_brightness: present gamma 1/brightness, 1 = the game's own output.
  f32 Brightness() const;
  // mgr_sharpening: contrast adaptive sharpening in the present pass, 0..1.
  f32 Sharpness() const;
  // mgr_upscaler: the scene's input size per axis as a fraction of the
  // output, 1 when off.
  f32 UpscalerInputFraction() const;
  bool PSOPrecache() const { return false; }
  bool GeometryGPUUpload() const { return false; }
  bool SceneColorR11G11B10() const { return false; }
};

// Internal resolution scale applied to the game's scene render targets
// (mgr_resolution_scale; 1 = the game's 1280x720).
f32 SceneRenderScale();
inline constexpr f32 kMinSceneRenderScale = 1.0f;
inline constexpr f32 kMaxSceneRenderScale = 3.0f;
inline constexpr u32 kGameWidth = 1280;
inline constexpr u32 kGameHeight = 720;
inline constexpr u32 kMinScaledTargetWidth = 320;
// MGR draws its 3D scene at 1152x720 and composites it into 1280x720.
inline constexpr u32 kSceneWidth = 1152;
// The scale the 3D scene renders at: SceneRenderScale(), or the upscaler's
// input fraction of it while mgr_upscaler is on.
f32 SceneInputScale();
// True while the scene renders at SceneInputScale() and is upscaled to
// SceneRenderScale() before the composite.
bool UpscalerActive();
// The largest output/input ratio the upscaler supports; 1 (the default)
// leaves it off.
void SetMaxUpscaleRatio(f32 ratio);
// Scale of a scene-shaped render target ('surface') or texture of these guest
// dimensions.
f32 SceneTargetScale(u32 width, u32 height, bool depth, bool surface);
// The 16:9 shapes (1280x720 and its halves) whose colour textures take the
// output scale while upscaling.
bool IsUpscaledShape(u32 width, u32 height);
// Bumped whenever the scene scale, shadow scale or MSAA count changes; targets
// built at an older generation are rebuilt before their next use.
u32 RenderTargetGeneration();
// Re-reads mgr_resolution_scale, mgr_shadow_resolution, mgr_msaa and
// mgr_upscaler; call once per frame on the render thread, so a frame's
// targets all agree.
void RefreshRenderTargetSettings();
// mgr_msaa as of the last refresh.
i32 ActiveMSAA();
// True for render targets of these guest dimensions that follow the scale.
bool IsSceneShapedTarget(u32 width, u32 height);
// The game's shadow caster surfaces (1024x1024) and the cascade atlas they
// resolve into (1024x2048) render at ShadowRenderScale().
inline constexpr u32 kShadowMapSize = 1024;
bool IsShadowShapedTarget(u32 width, u32 height);
// mgr_shadow_resolution / 1024 as of the last refresh; 1 keeps the original.
f32 ShadowRenderScale();
// A guest extent in host pixels, at least 1.
u32 ScaleExtent(u32 extent, f32 scale);
// Shadow-distance multiplier the shadow PCF kernel compensates for. MGR does
// not change shadow distance, so it stays 1.
inline f64 ShadowCoverageScale() { return 1.0; }

} // namespace mgrn::gpu
