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
  // mgr_msaa: 0/2/4/8, latched when the device is created.
  i32 MSAA() const;
  bool PSOPrecache() const { return false; }
  bool GeometryGPUUpload() const { return false; }
  bool SceneColorR11G11B10() const { return false; }
};

// Internal resolution scale applied to the game's scene render targets
// (mgr_resolution_scale; 1 = the game's 1280x720).
f32 SceneRenderScale();
// Shadow-distance multiplier the shadow PCF kernel compensates for. MGR does
// not change shadow distance, so it stays 1.
inline f64 ShadowCoverageScale() { return 1.0; }

} // namespace mgrn::gpu
