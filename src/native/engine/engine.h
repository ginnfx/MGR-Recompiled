/**
 * @file    engine/engine.h
 * @brief   Stand-ins for the re:Blue engine queries the ported present path
 *          makes. MGR paces frames with its own vblank timing and plays
 *          movies through the normal draw path, so these report "no override".
 */
#pragma once

#include <rex/types.h>

namespace mgrn::engine {

struct Settings {
  static Settings &Get() {
    static Settings settings;
    return settings;
  }
  // Host-side frame cap; 0 = none (the guest's vblank pacing decides).
  i32 FPSLimit() const { return 0; }
};

struct SofdecPlayer {
  static bool Playing() { return false; }
};

} // namespace mgrn::engine
