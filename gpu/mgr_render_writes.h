#pragma once
#include <cstdint>

namespace mgr {
// The same normalized write controls feed Vulkan pipeline construction. Keep
// all stencil-enabled draws conservative, even if individual ops/masks keep it.
// Attachment binding alone does not imply a store: the RT cache accumulates
// bindings across draws specifically to avoid breaking render passes.
constexpr uint32_t RenderTargetWriteMask(bool rasterization, bool depth_write,
                                         bool stencil_enabled, uint32_t color_mask) {
  if (!rasterization) return 0;
  uint32_t result = depth_write || stencil_enabled ? 1u : 0u;
  for (uint32_t i = 0; i < 4; ++i)
    if ((color_mask >> (4*i)) & 15u) result |= 2u << i;
  return result;
}
}  // namespace mgr
