#pragma once

#include <rex/graphics/pipeline/texture/info.h>

namespace mgr::metal4 {

// The shader performs the image-view swizzle because Metal guest textures are
// bound without Xenos-compatible texture views. Match the host format swizzle
// selected by VulkanTextureCache before applying the guest fetch swizzle.
inline uint32_t ShaderTextureSwizzle(rex::graphics::xenos::TextureFormat format,
                                     uint32_t guest_swizzle) {
  using namespace rex::graphics;
  using xenos::TextureFormat;
  uint32_t host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  switch (GetBaseFormat(format)) {
    case TextureFormat::k_8:
    case TextureFormat::k_16:
    case TextureFormat::k_16_FLOAT:
    case TextureFormat::k_24_8:
    case TextureFormat::k_24_8_FLOAT:
    case TextureFormat::k_32_FLOAT:
      host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR;
      break;
    case TextureFormat::k_8_8:
    case TextureFormat::k_16_16:
    case TextureFormat::k_16_16_FLOAT:
    case TextureFormat::k_32_32_FLOAT:
    case TextureFormat::k_DXN:
      host_swizzle = xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG;
      break;
    default:
      break;
  }
  uint32_t shader_swizzle = 0;
  for (uint32_t channel = 0; channel < 4; ++channel) {
    const uint32_t guest_channel = (guest_swizzle >> (3 * channel)) & 7;
    const uint32_t host_channel =
        guest_channel >= xenos::XE_GPU_TEXTURE_SWIZZLE_0
            ? (guest_channel & 5)
            : ((host_swizzle >> (3 * guest_channel)) & 7);
    shader_swizzle |= host_channel << (3 * channel);
  }
  return shader_swizzle;
}

}  // namespace mgr::metal4
