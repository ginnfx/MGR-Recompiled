/**
 * @file    gpu/settings.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Adapted for MGR (see settings.h).
 */
#include "gpu/settings.h"

#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(mgr_msaa, 0, "MGR/Graphics",
                     "Native renderer multisample count for scene render targets: 0, 2, 4 or 8. "
                     "Applies on the next launch.");
REXCVAR_DEFINE_INT32(mgr_shadow_resolution, 0, "MGR/Graphics",
                     "Native renderer shadow map size in texels; 0 keeps the game's size.");

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

f32 SceneRenderScale() {
  try {
    const f32 scale = std::stof(Flag("mgr_resolution_scale"));
    return scale > 0.0f ? scale : 1.0f;
  } catch (...) {
    return 1.0f;
  }
}

} // namespace mgrn::gpu
