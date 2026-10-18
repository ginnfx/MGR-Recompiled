#pragma once

#include <cstdint>
#include <cstring>

namespace mgr::metal4 {

// Copies one guest texture block into the linear upload buffer.
//
// Endianness permutes components inside uncompressed pixels. Block-compressed
// formats (DXT1/2/3/4/5, DXN) are byte streams of BC blocks; XORing each byte
// of a block corrupts it, so BC blocks are preserved byte for byte.
inline void CopyGuestTextureBlock(bool block_compressed, uint32_t endianness,
                                  const uint8_t* source, uint8_t* destination,
                                  uint32_t block_bytes) {
  if (block_compressed) {
    std::memcpy(destination, source, block_bytes);
    return;
  }
  constexpr uint32_t lane_xor[] = {0, 1, 3, 2};
  const uint32_t lane = lane_xor[endianness & 3];
  for (uint32_t byte = 0; byte < block_bytes; ++byte)
    destination[byte] = source[byte ^ lane];
}

}  // namespace mgr::metal4
