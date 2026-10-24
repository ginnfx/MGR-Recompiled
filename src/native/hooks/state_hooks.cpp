// Native renderer: D3D state setters.
//
// Each hook runs the guest setter first, so the guest device (register images,
// bound-object shadows, dirty masks) stays exactly as the game expects, then
// mirrors the binding into the plume renderer's state. Render state proper
// (blend, depth, stencil, cull) is not hooked: the draw reads it from the
// device's register images (gpu/draw.cpp ReadDeviceRenderState). Like re:Blue,
// the mirrors run on the guest render thread without state().mutex: only the
// recording paths (draw, upload, present) take it.
// Adapted from re:Blue gpu/hooks/state.cpp (BSD-3, Copyright (c) 2026 Tom Clay).

#include <atomic>
#include <format>
#include <string>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/format.h"
#include "gpu/host_resource_heap.h"
#include "gpu/physical_buffers.h"
#include "gpu/shaders/shader_constants.h"
#include "hooks/hook_macros.h"
#include "hooks/resources.h"

namespace {

using mgrn::NativeRendererActive;
namespace gpu = mgrn::gpu;

void NoteExtraRenderTarget(u32 index, u32 surface_va) {
  static std::atomic<u32> logged{0};
  if (surface_va && logged.fetch_add(1, std::memory_order_relaxed) < 16)
    MGRN_WARN("native: SetRenderTarget({}) 0x{:08X} ignored, MRT is not modeled", index,
              surface_va);
}

void MirrorRenderTarget(u32 device_va, u32 index, u32 surface_va) {
  // MRT is not modeled: only RT0 drives the framebuffer.
  if (index != 0) return NoteExtraRenderTarget(index, surface_va);
  gpu::GuestTexture* surface = mgrn::native::ResolveSurface(surface_va);
  auto& s = gpu::state();
  if (s.render_target != surface) s.draw_framebuffer_bound = false;
  gpu::Video::SetDirtyValue<gpu::GuestTexture*>(s.dirtyStates.renderTargetAndDepthStencil,
                                                s.render_target, surface);
  gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.renderTargetFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  gpu::Video::SetDirtyValue<plume::RenderSampleCounts>(
      s.dirtyStates.pipelineState, s.pipelineState.sampleCount,
      surface ? surface->sampleCount : plume::RenderSampleCount::COUNT_1);
  gpu::Video::SetDefaultViewport(mgrn::mem::at<gpu::D3DDevice>(device_va), surface);
}

void MirrorDepthStencil(u32 device_va, u32 surface_va) {
  gpu::GuestTexture* surface = mgrn::native::ResolveSurface(surface_va);
  if (surface && !gpu::IsDepthFormat(surface->format)) surface = nullptr;
  auto& s = gpu::state();
  if (s.depth_stencil != surface) s.draw_framebuffer_bound = false;
  gpu::Video::SetDirtyValue<gpu::GuestTexture*>(s.dirtyStates.renderTargetAndDepthStencil,
                                                s.depth_stencil, surface);
  gpu::Video::SetDirtyValue<plume::RenderFormat>(
      s.dirtyStates.pipelineState, s.pipelineState.depthStencilFormat,
      surface ? surface->format : plume::RenderFormat::UNKNOWN);
  if (surface) s.scene_depth = surface;
  (void)device_va;
}

void MirrorViewport(u32 viewport_va) {
  const auto* vp = mgrn::mem::at<const gpu::D3DViewport9>(viewport_va);
  if (!vp) return;
  auto& s = gpu::state();
  bool& dirty = s.dirtyStates.viewport;
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.x, float(u32(vp->X)));
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.y, float(u32(vp->Y)));
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.width, float(u32(vp->Width)));
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.height, float(u32(vp->Height)));
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.minDepth, float(vp->MinZ));
  gpu::Video::SetDirtyValue<float>(dirty, s.viewport.maxDepth, float(vp->MaxZ));
  s.dirtyStates.scissorRect = s.dirtyStates.scissorRect || dirty;
}

void MirrorVertexDeclaration(u32 decl_va) {
  auto* decl = gpu::HostResourceHeap::FromGuest<gpu::GuestVertexDeclaration>(decl_va);
  {
    // The recompiled VS guards each semantic's R11G11B10 decode on its own bit.
    auto& s = gpu::state();
      u32 spec = s.pipelineState.specConstants & ~gpu::kSpecConstantR11G11B10Mask;
    if (decl) spec |= decl->r11g11b10Spec;
    gpu::Video::SetDirtyValue<u32>(s.dirtyStates.pipelineState, s.pipelineState.specConstants,
                                   spec);
  }
  gpu::Video::SetVertexDeclaration(decl);
}

// Geometry bound through the hooks, so a draw can re-copy what the guest
// rewrote while it stayed bound. Guest render thread only, like the setters.
constexpr u32 kStreamCount = 16;
// Draw-time header re-points logged; later ones are not reported.
constexpr u32 kMaxRepointLogs = 32;
struct BoundStream {
  gpu::GuestBuffer* buffer = nullptr;
  u32 offset = 0;
  u32 stride = 0;
};
BoundStream g_streams[kStreamCount];
gpu::GuestBuffer* g_indices = nullptr;

void BindVertexStream(u32 stream, gpu::GuestBuffer* buf, u32 offset, u32 stride) {
  if (stream < kStreamCount) g_streams[stream] = {buf, offset, stride};
  if (buf && buf->hasBuffer()) {
    const u32 bound_size = offset < buf->dataSize ? buf->dataSize - offset : 0;
    gpu::Video::SetVertexStream(stream, buf->bufferRef(offset), bound_size, stride);
  } else {
    gpu::Video::SetVertexStream(stream, plume::RenderBufferReference{}, 0, 0);
  }
}

// Re-copies 'buf' if the guest rewrote it, rebinding every view of the old
// copy, which is freed once the frames reading it retire.
void RefreshAndRebind(gpu::GuestBuffer* buf) {
  if (!gpu::RefreshIfWritten(buf)) return;
  for (u32 i = 0; i < kStreamCount; ++i) {
    const BoundStream bound = g_streams[i];
    if (bound.buffer == buf) BindVertexStream(i, buf, bound.offset, bound.stride);
  }
  if (g_indices == buf) gpu::Video::SetIndices(buf);
}

void MirrorStreamSource(u32 stream, u32 buffer_va, u32 offset, u32 stride) {
  auto* buf = buffer_va
                  ? gpu::ResolveGuestBufferVa(buffer_va, gpu::ResourceType::VertexBuffer)
                  : nullptr;
  RefreshAndRebind(buf);
  BindVertexStream(stream, buf, offset, stride);
}

void MirrorIndices(u32 indices_va) {
  auto* buf =
      indices_va ? gpu::ResolveGuestBufferVa(indices_va, gpu::ResourceType::IndexBuffer) : nullptr;
  RefreshAndRebind(buf);
  g_indices = buf;
  gpu::Video::SetIndices(buf);
}

// Bit i set when the bound declaration fetches from stream i. All streams when
// no declaration is bound.
u32 DeclaredStreamMask() {
  auto& s = gpu::state();
  std::lock_guard lock(s.mutex);
  const auto* decl = s.pipelineState.vertexDeclaration;
  if (!decl || !decl->vertexElements) return (1u << kStreamCount) - 1;
  u32 mask = 0;
  for (u32 i = 0; i < decl->vertexElementCount; ++i)
    if (u32 stream = u32(decl->vertexElements[i].stream); stream < kStreamCount)
      mask |= 1u << stream;
  return mask;
}

// Drops raw pointers to a buffer the physical registry is about to destroy.
void ForgetEvictedBuffer(const gpu::GuestBuffer* buf) {
  for (u32 i = 0; i < kStreamCount; ++i)
    if (g_streams[i].buffer == buf) BindVertexStream(i, nullptr, 0, 0);
  if (g_indices == buf) {
    g_indices = nullptr;
    gpu::Video::SetIndices(nullptr);
  }
}

const bool g_evicted_listener_set =
    (gpu::SetGuestBufferEvictedListener(&ForgetEvictedBuffer), true);

}  // namespace

void mgrn::native::RefreshBoundGeometry(u32 device_guest, bool indexed) {
  // The engine re-points buffer headers at other memory (double-buffered
  // dynamic geometry) without setting them again, so re-resolve each bound
  // header from the device before the draw, as the GPU would read it. Only
  // streams the declaration fetches (and the index buffer of an indexed
  // draw): a slot left bound from an earlier draw may hold a freed header.
  const u32 streams = DeclaredStreamMask();
  if (const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest)) {
    static std::atomic<u32> repoints{0};
    for (u32 i = 0; i < kStreamCount; ++i) {
      BoundStream& bound = g_streams[i];
      const u32 header = u32(dev->streamSources[i]);
      if (!(streams & (1u << i)) || !bound.buffer || !header)
        continue;
      auto* current = gpu::ResolveGuestBufferVa(header, gpu::ResourceType::VertexBuffer);
      if (current && current != bound.buffer) {
        if (repoints.fetch_add(1, std::memory_order_relaxed) < kMaxRepointLogs)
          MGRN_INFO("native: stream {} header 0x{:08X} re-pointed 0x{:08X} -> 0x{:08X} "
                    "since it was set",
                    i, header, bound.buffer->guestMirrorVa, current->guestMirrorVa);
        BindVertexStream(i, current, bound.offset, bound.stride);
      }
    }
    const u32 ib_header = u32(dev->indexBuffer);
    if (indexed && g_indices && ib_header) {
      auto* current = gpu::ResolveGuestBufferVa(ib_header, gpu::ResourceType::IndexBuffer);
      if (current && current != g_indices) {
        if (repoints.fetch_add(1, std::memory_order_relaxed) < kMaxRepointLogs)
          MGRN_INFO("native: index header 0x{:08X} re-pointed 0x{:08X} -> 0x{:08X} since it "
                    "was set",
                    ib_header, g_indices->guestMirrorVa, current->guestMirrorVa);
        g_indices = current;
        gpu::Video::SetIndices(current);
      }
    }
  }
  for (u32 i = 0; i < kStreamCount; ++i)
    if (streams & (1u << i)) RefreshAndRebind(g_streams[i].buffer);
  if (indexed) RefreshAndRebind(g_indices);
}

std::string mgrn::native::DescribeBoundStreams(u32 device_guest, u32 stream_count) {
  // Adopted buffers are keyed by their struct VA, so compare host objects.
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  auto describe = [](const char* name, u32 guest, const gpu::GuestBuffer* ours) {
    const gpu::GuestBuffer* expected = guest ? gpu::FindPhysicalBufferByStruct(guest) : nullptr;
    return std::format(" {}=0x{:08X}{}", name, guest,
                       expected == ours ? "" : (expected ? "!STALE" : "!UNADOPTED"));
  };
  // First bytes of the bound data, as the guest stored them (big-endian).
  auto head_bytes = [](const gpu::GuestBuffer* buf, u32 offset) {
    constexpr u32 kHeadBytes = 32;
    if (!buf || !buf->guestMirrorVa || offset + kHeadBytes > buf->dataSize)
      return std::string();
    const auto* src = mgrn::mem::try_at<const u8>(buf->guestMirrorVa + offset);
    if (!src)
      return std::string();
    std::string hex = " [";
    for (u32 b = 0; b < kHeadBytes; ++b)
      hex += std::format("{}{:02X}", (b && b % 4 == 0) ? " " : "", src[b]);
    return hex + "]";
  };
  std::string out;
  for (u32 i = 0; i < stream_count && i < kStreamCount; ++i) {
    const BoundStream& bound = g_streams[i];
    out += describe(std::format("s{}", i).c_str(), dev ? u32(dev->streamSources[i]) : 0,
                    bound.buffer);
    if (bound.buffer)
      out += std::format(" base=0x{:08X}+{} stride={} size={}{}", bound.buffer->guestMirrorVa,
                         bound.offset, bound.stride, bound.buffer->dataSize,
                         head_bytes(bound.buffer, bound.offset));
  }
  out += describe("ib", dev ? u32(dev->indexBuffer) : 0, g_indices);
  if (g_indices)
    out += std::format(" base=0x{:08X} size={} elem={}{}", g_indices->guestMirrorVa,
                       g_indices->dataSize, g_indices->elementSize, head_bytes(g_indices, 0));
  return out;
}

// --- Render targets ----------------------------------------------------------

MGRN_EXTERN(D3DDevice_SetRenderTarget);
MGRN_HOOK_RAW(D3DDevice_SetRenderTarget) {
  const u32 device = ctx.r3.u32, index = ctx.r4.u32, surface = ctx.r5.u32;
  MGRN_IMP(D3DDevice_SetRenderTarget)(ctx, base);
  if (NativeRendererActive()) MirrorRenderTarget(device, index, surface);
}

MGRN_EXTERN(D3DDevice_SetDepthStencilSurface);
MGRN_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  const u32 device = ctx.r3.u32, surface = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetDepthStencilSurface)(ctx, base);
  if (NativeRendererActive()) MirrorDepthStencil(device, surface);
}

MGRN_EXTERN(D3DDevice_SetViewport);
MGRN_HOOK_RAW(D3DDevice_SetViewport) {
  const u32 viewport = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetViewport)(ctx, base);
  if (NativeRendererActive()) MirrorViewport(viewport);
}

MGRN_EXTERN(D3DDevice_SetScissorRect);
MGRN_HOOK_RAW(D3DDevice_SetScissorRect) {
  MGRN_IMP(D3DDevice_SetScissorRect)(ctx, base);
  // The scissor follows the viewport extent (Video::FlushViewport); only the
  // dirty mark matters.
  if (NativeRendererActive()) gpu::state().dirtyStates.scissorRect = true;
}

// --- Shaders and vertex input -------------------------------------------------

MGRN_EXTERN(D3DDevice_SetVertexShader);
MGRN_HOOK_RAW(D3DDevice_SetVertexShader) {
  const u32 shader = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetVertexShader)(ctx, base);
  if (NativeRendererActive())
    gpu::Video::SetVertexShader(gpu::HostResourceHeap::FromGuest<gpu::GuestShader>(shader));
}

MGRN_EXTERN(D3DDevice_SetPixelShader);
MGRN_HOOK_RAW(D3DDevice_SetPixelShader) {
  const u32 shader = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetPixelShader)(ctx, base);
  if (NativeRendererActive())
    gpu::Video::SetPixelShader(gpu::HostResourceHeap::FromGuest<gpu::GuestShader>(shader));
}

MGRN_EXTERN(D3DDevice_SetVertexDeclaration);
MGRN_HOOK_RAW(D3DDevice_SetVertexDeclaration) {
  const u32 decl = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetVertexDeclaration)(ctx, base);
  if (NativeRendererActive()) MirrorVertexDeclaration(decl);
}

MGRN_EXTERN(D3DDevice_SetStreamSource);
MGRN_HOOK_RAW(D3DDevice_SetStreamSource) {
  const u32 stream = ctx.r4.u32, buffer = ctx.r5.u32, offset = ctx.r6.u32,
            stride = ctx.r7.u32;
  MGRN_IMP(D3DDevice_SetStreamSource)(ctx, base);
  if (NativeRendererActive()) MirrorStreamSource(stream, buffer, offset, stride);
}

MGRN_EXTERN(D3DDevice_SetIndices);
MGRN_HOOK_RAW(D3DDevice_SetIndices) {
  const u32 indices = ctx.r4.u32;
  MGRN_IMP(D3DDevice_SetIndices)(ctx, base);
  if (NativeRendererActive()) MirrorIndices(indices);
}

// --- Textures -------------------------------------------------------------------

MGRN_EXTERN(D3DDevice_SetTexture);
MGRN_HOOK_RAW(D3DDevice_SetTexture) {
  const u32 sampler = ctx.r4.u32, texture = ctx.r5.u32;
  MGRN_IMP(D3DDevice_SetTexture)(ctx, base);
  if (!NativeRendererActive())
    return;
  gpu::GuestTexture* host = mgrn::native::ResolveTexture(texture);
  // A real texture the mirror could not build (the k_24_8_FLOAT shadow map) must not
  // leave the slot's previous texture bound for the shader to read.
  if (texture && !host)
    gpu::Video::UnbindTexture(sampler);
  else
    gpu::Video::SetTexture(sampler, host);
}
