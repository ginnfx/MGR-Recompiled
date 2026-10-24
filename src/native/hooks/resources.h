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
struct GuestBuffer;
struct GuestTexture;
struct GuestVertexDeclaration;
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

// Re-resolves the bound vertex/index buffer headers from the device (the
// engine re-points them) and re-copies buffers the guest has rewritten since.
// Call before each draw.
void RefreshBoundGeometry(u32 device_guest, bool indexed);

// The vertex streams and index buffer the hooks bound for the next draw.
constexpr u32 kBoundStreamCount = 16;
struct BoundStreamInfo {
  const gpu::GuestBuffer* buffer = nullptr;
  u32 offset = 0;
  u32 stride = 0;
};
BoundStreamInfo BoundStreamAt(u32 stream);
const gpu::GuestBuffer* BoundIndexBuffer();

// Dev: true when a bound vertex stream or the index buffer mirrors the guest
// buffer at base address 'guest_va'.
bool DrawReadsGuestBuffer(u32 guest_va);

// Dev: for streams 0..stream_count-1 and the index buffer, the device's bound
// guest pointer, flagged !STALE when the hooks track another buffer for it.
std::string DescribeBoundStreams(u32 device_guest, u32 stream_count);

// Dev: logs (rate-limited) a draw whose highest vertex fetch on any stream
// the declaration reads lies past that stream's bound buffer, where the host
// fetch returns zeros (a vertex at w=0 stretches to infinity).
void CheckStreamBounds(const gpu::GuestVertexDeclaration* decl, bool indexed, u32 start,
                       u32 count, i32 base_vertex, u32 present);

// Dev: for a skinned draw, logs (rate-limited) vertices whose blend weights do
// not sum to one and referenced bone matrices that are not finite or have
// implausible magnitudes, read from the device constant shadow.
void CheckSkinSanity(u32 device_guest, const gpu::GuestVertexDeclaration* decl, bool indexed,
                     u32 start, u32 count, i32 base_vertex, u32 palette_first_reg,
                     u32 palette_regs, u32 present, u64 vs_hash);

// Dev: remembers each indexed draw's index range by (buffer, start, count) and
// logs (rate-limited) when its content differs from the previous frame's,
// with how many zero indices each version holds.
void CheckIndexStability(bool indexed, u32 start, u32 count, u32 present);

// Dev: hashes the guest bytes behind streams 0..1 and the index buffer for
// draw #draw_index; ReportLateGeometryWrites (at Swap) rehashes them and logs
// every range the guest rewrote after the draw was issued.
void NoteDrawGeometry(u32 draw_index);
void ReportLateGeometryWrites();

// Dev: for a skinned draw, walks the vertices its indices reach in the guest
// data and counts weighted bone references past the palette or to an all-zero
// bone, plus the largest FLOAT3 position component.
std::string CheckSkinReferences(u32 device_guest, const gpu::GuestVertexDeclaration* decl,
                                bool indexed, u32 start, u32 count, i32 base_vertex,
                                u32 palette_first_reg, u32 palette_regs);

}  // namespace mgrn::native
