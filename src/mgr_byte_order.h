// Big-endian reads for the Xbox 360 file formats.
#pragma once

#include <cstdint>

namespace mgr_install {

// The caller checks that p has four readable bytes.
[[nodiscard]] inline uint32_t ReadBE32(const uint8_t* p) {
  return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | uint32_t{p[3]};
}

}  // namespace mgr_install
