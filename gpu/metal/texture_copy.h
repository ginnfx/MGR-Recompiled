#pragma once

#include <cstdint>
#include <cstring>

namespace mgr::metal4 {

// Copies one guest texture block into the linear upload buffer.
//
// Xenos endianness is applied within each 32-bit word before BC decoding,
// matching the Vulkan texture-load shader. Never permute across BC blocks.
inline void CopyGuestTextureBlock(bool block_compressed, uint32_t endianness,
                                  const uint8_t* source, uint8_t* destination,
                                  uint32_t block_bytes) {
  if (block_compressed) {
    constexpr uint32_t lane_xor[] = {0, 1, 3, 2};
    const uint32_t lane = lane_xor[endianness & 3];
    for (uint32_t word = 0; word < block_bytes; word += 4) {
      for (uint32_t byte = 0; byte < 4; ++byte)
        destination[word + byte] = source[word + (byte ^ lane)];
    }
    return;
  }
  constexpr uint32_t lane_xor[] = {0, 1, 3, 2};
  const uint32_t lane = lane_xor[endianness & 3];
  for (uint32_t byte = 0; byte < block_bytes; ++byte)
    destination[byte] = source[byte ^ lane];
}

}  // namespace mgr::metal4
