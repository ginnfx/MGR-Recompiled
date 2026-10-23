// Native renderer: draws, clears, resolves and present.
//
// Draw, Clear and Resolve replace the guest bodies (which would only build PM4
// for the emulator); Swap presents and then runs the guest body, so the flip
// queue, VdSwap and the game's frame pacing behave exactly as before.
// DispatchDraw adapted from re:Blue gpu/hooks/draw.cpp (BSD-3, Copyright (c)
// 2026 Tom Clay).

#include <atomic>
#include <mutex>

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/host_resource_heap.h"
#include "hooks/hook_macros.h"
#include "hooks/resources.h"

namespace {

namespace xe = rex::graphics::xenos;
namespace gpu = mgrn::gpu;
using mgrn::NativeRendererActive;

struct DrawArgs {
  bool indexed = false;
  u32 vertex_or_index_count = 0;
  i32 base_vertex_index = 0;
  u32 start_index = 0;
  u32 start_vertex = 0;
};

plume::RenderPrimitiveTopology MapPrimitiveType(u32 prim) {
  switch (static_cast<xe::PrimitiveType>(prim)) {
    case xe::PrimitiveType::kPointList:
      return plume::RenderPrimitiveTopology::POINT_LIST;
    case xe::PrimitiveType::kLineList:
      return plume::RenderPrimitiveTopology::LINE_LIST;
    case xe::PrimitiveType::kLineStrip:
      return plume::RenderPrimitiveTopology::LINE_STRIP;
    case xe::PrimitiveType::kTriangleList:
    case xe::PrimitiveType::kQuadList:  // expanded through the quad index buffer
      return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    case xe::PrimitiveType::kTriangleFan:
      return plume::RenderPrimitiveTopology::TRIANGLE_FAN;
    case xe::PrimitiveType::kTriangleStrip:
      return plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
    default: {
      static std::atomic<u32> warned{0};
      if (warned.fetch_add(1, std::memory_order_relaxed) < 8)
        MGRN_WARN("native draw: primitive type {} not implemented, drawn as a triangle list",
                  prim);
      return plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    }
  }
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const DrawArgs& args) {
  mgrn::native::RefreshBoundGeometry();
  auto& s = gpu::state();
  std::unique_lock<std::mutex> lock(s.mutex);
  gpu::Video::OpenCommandListLocked();

  // PSO key includes topology, so set it before any flush.
  gpu::Video::SetDirtyValue<plume::RenderPrimitiveTopology>(
      s.dirtyStates.pipelineState, s.pipelineState.primitiveTopology,
      MapPrimitiveType(primitive_type));
  // The engine writes constant shadows directly as well as through the
  // setters, so upload both stages on every draw.
  s.dirtyStates.vertexShaderConstants = true;
  s.dirtyStates.pixelShaderConstants = true;

  if (!gpu::Video::BindDrawFramebufferLocked()) return;
  if (!gpu::Video::FlushRenderStateLocked(device_guest)) return;

  auto* cmd_list = s.command_list;
  if (!cmd_list) return;

  if (static_cast<xe::PrimitiveType>(primitive_type) == xe::PrimitiveType::kQuadList) {
    const u32 quads = std::min(args.vertex_or_index_count / 4, gpu::Video::QuadlistMaxQuads());
    const auto* ib = gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      const i32 first = args.indexed ? args.base_vertex_index : i32(args.start_vertex);
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, first, 0);
      // The expansion IB replaced the tracked guest IB on the command list.
      s.dirtyStates.indices = true;
    }
  } else if (args.indexed) {
    if (s.index_view.buffer.ref == nullptr) {
      static std::atomic<u32> warned{0};
      if (warned.fetch_add(1, std::memory_order_relaxed) < 16)
        MGRN_WARN("native draw: indexed draw with no index buffer (count={} start={} base={})",
                  args.vertex_or_index_count, args.start_index, args.base_vertex_index);
      return;
    }
    cmd_list->drawIndexedInstanced(args.vertex_or_index_count, 1, args.start_index,
                                   args.base_vertex_index, 0);
  } else {
    cmd_list->drawInstanced(args.vertex_or_index_count, 1, args.start_vertex, 0);
  }
}

}  // namespace

// D3DDevice_DrawIndexedVertices(pDevice, PrimitiveType, BaseVertexIndex,
// StartIndex, IndexCount).
MGRN_EXTERN(D3DDevice_DrawIndexedVertices);
MGRN_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_DrawIndexedVertices)(ctx, base);
  DrawArgs args;
  args.indexed = true;
  args.base_vertex_index = i32(ctx.r5.u32);
  args.start_index = ctx.r6.u32;
  args.vertex_or_index_count = ctx.r7.u32;
  DispatchDraw(ctx.r3.u32, ctx.r4.u32, args);
}

// D3DDevice_DrawVertices(pDevice, PrimitiveType, StartVertex, VertexCount).
MGRN_EXTERN(D3DDevice_DrawVertices);
MGRN_HOOK_RAW(D3DDevice_DrawVertices) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_DrawVertices)(ctx, base);
  DrawArgs args;
  args.start_vertex = ctx.r5.u32;
  args.vertex_or_index_count = ctx.r6.u32;
  DispatchDraw(ctx.r3.u32, ctx.r4.u32, args);
}

// D3DDevice_Clear(pDevice, Count, pRects, Flags, Color, Z, Stencil). Z travels
// in f1 but still consumes the r8 slot, so Stencil is r9.
MGRN_EXTERN(D3DDevice_Clear);
MGRN_HOOK_RAW(D3DDevice_Clear) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_Clear)(ctx, base);
  gpu::Video::Clear(ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r9.u32);
}

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint,
// DestLevel, DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters).
MGRN_EXTERN(D3DDevice_Resolve);
MGRN_HOOK_RAW(D3DDevice_Resolve) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_Resolve)(ctx, base);
  const u32 flags = ctx.r4.u32, dest = ctx.r6.u32, level = ctx.r8.u32, slice = ctx.r9.u32;
  gpu::GuestTexture* dst = mgrn::native::ResolveDestination(dest);
  if (!dst) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 16)
      MGRN_WARN("native resolve: destination 0x{:08X} has no host texture", dest);
    return;
  }
  gpu::Video::TrackResolveSource(flags, dst, level, slice);
  gpu::Video::ResolveRtToTexture(dst);
}

// D3DDevice_Swap(pDevice, pFrontBuffer, pParameters): pFrontBuffer is the
// texture the engine just resolved the finished frame into.
MGRN_EXTERN(D3DDevice_Swap);
MGRN_HOOK_RAW(D3DDevice_Swap) {
  if (NativeRendererActive())
    gpu::Video::Present(mgrn::native::LookupPresentTexture(ctx.r4.u32));
  MGRN_IMP(D3DDevice_Swap)(ctx, base);
}
