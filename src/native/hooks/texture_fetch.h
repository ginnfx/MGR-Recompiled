// Native renderer: the texture fetch constant inside a guest D3DTexture.
//
// The guest's D3D runtime fills each D3DTexture header with the Xenos fetch
// constant the GPU would sample through, so it describes any texture the
// engine built, including the ones it set up by hand (resolve targets, views
// aliasing another texture's memory) without a D3DDevice_CreateTexture call.
#pragma once

#include <string>

#include <rex/graphics/xenos.h>
#include <rex/types.h>

namespace mgrn::native {

struct TextureFetchDesc {
  rex::graphics::xenos::TextureFormat format{};
  rex::graphics::xenos::DataDimension dimension{};
  rex::graphics::xenos::Endian endian{};
  bool tiled = false;
  u32 base_address = 0;  // guest physical address of mip 0
  u32 width = 0, height = 0, depth = 1;
  u32 levels = 1;
  // Bytes of guest memory mip 0 (every face or slice) spans from base_address.
  u32 size_bytes = 0;
  // A D3DFORMAT word with the fetch's format, endian, tiling and sign fields,
  // for the D3DFORMAT-keyed conversions (gpu::ConvertGuestFormat).
  u32 d3d_format = 0;
};

// False when the VA does not translate or does not hold a texture fetch.
bool DecodeTextureFetch(u32 texture_va, TextureFetchDesc& out);

// One-line summary for logs.
std::string DescribeFetch(const TextureFetchDesc& desc);

}  // namespace mgrn::native
