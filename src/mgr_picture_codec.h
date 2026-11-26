// Encodes RGBA images into the Xbox 360 texture layout the retail UI samples:
// DXT5 (BC3) blocks, 16-bit byte swapped, in the console's tiled block order.
// The layout matches tools/assets/prepare_ui_font.py, which decodes the same
// textures out of the game's .wtb files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mgr_pictures {

// Tiled block offset in bytes for 16 byte blocks; `pitch_blocks` is the row length
// in 4x4 blocks (rounded up to 32 like the hardware does).
uint32_t TiledBlockOffset(uint32_t block_x, uint32_t block_y, uint32_t pitch_blocks);

// Size in bytes of a tiled DXT5 texture (dimensions rounded up to 128 texels so
// the tiling covers whole 32x32 block tiles).
size_t TiledDxt5Size(uint32_t width, uint32_t height);

// `rgba` holds width*height pixels, 4 bytes each (R, G, B, A, straight alpha).
// Width and height must be multiples of 128. Returns TiledDxt5Size(...) bytes in
// guest (big endian) order, ready to copy into the texture's guest memory.
std::vector<uint8_t> EncodeTiledDxt5(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height);

// A rectangle of texels; coordinates and sizes are multiples of 4 (whole blocks).
struct Region {
  uint32_t x, y, width, height;
};

// Encodes only the blocks inside `region` of the `width` x `height` RGBA image into
// `tiled` (TiledDxt5Size(width, height) bytes), leaving every other block as it is.
void EncodeTiledDxt5Region(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, Region region,
                           uint8_t* tiled);

// Copies the blocks inside `region` from `source` to `tiled` (both TiledDxt5Size bytes,
// same layout), leaving every other block of `tiled` as it is.
void CopyTiledRegion(const uint8_t* source, uint8_t* tiled, uint32_t width, uint32_t height, Region region);

// Inverse of EncodeTiledDxt5 for tests and diagnostics.
std::vector<uint8_t> DecodeTiledDxt5(const uint8_t* data, uint32_t width, uint32_t height);

}  // namespace mgr_pictures
