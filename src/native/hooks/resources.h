// Native renderer: host resources for the guest's own D3D objects.
//
// MGR keeps its D3D9 runtime, so the guest creates every texture, surface,
// shader and vertex declaration itself. The create hooks below attach a host
// object to each guest object VA (gpu::HostResourceHeap); objects the engine
// builds without a create call (asset textures, physical vertex/index buffers)
// are adopted lazily from their fetch constants.
#pragma once

#include <string>

#include <rex/types.h>

namespace mgrn::gpu {
struct GuestTexture;
}

namespace mgrn::native {

// Render target or depth surface bound by SetRenderTarget/SetDepthStencilSurface.
gpu::GuestTexture* ResolveSurface(u32 surface_va);
// Texture bound by SetTexture: a resolve target's host texture, or a mirror of
// the guest texel data built from the texture's fetch constant.
gpu::GuestTexture* ResolveTexture(u32 texture_va);
// Destination of D3DDevice_Resolve: a host texture the resolve copy writes.
gpu::GuestTexture* ResolveDestination(u32 texture_va);
// Front buffer passed to D3DDevice_Swap: the host texture last resolved into
// it (directly or through another header over the same memory), or nullptr.
gpu::GuestTexture* LookupPresentTexture(u32 texture_va);

// Re-copies vertex and index buffers still bound from earlier setter calls
// that the guest has rewritten since. Call before each draw.
void RefreshBoundGeometry();

// Dev: for streams 0..stream_count-1 and the index buffer, the device's bound
// guest pointer, flagged !STALE when the hooks track another buffer for it.
std::string DescribeBoundStreams(u32 device_guest, u32 stream_count);

}  // namespace mgrn::native
