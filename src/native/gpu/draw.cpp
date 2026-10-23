/**
 * @file    gpu/draw.cpp
 * @brief   What every draw flushes: the guest render state read, the PSO
 *          lookup, and the constant buffer uploads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <cstddef>
#include <cstring>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/format.h"
#include "gpu/pipeline/pipeline_cache.h"
#include "gpu/pipeline/pso_recorder.h"

namespace mgrn::gpu {
namespace {

// MGR: render state is decoded from the Xenos register images the guest D3D
// setters keep in the device (D3DDevice::regs*, ReadDeviceRegister), so every
// setter path, inlined or not, is covered.
constexpr u32 kRegColorMask = 0x2104;        // RB_COLOR_MASK
constexpr u32 kRegStencilRefMaskBf = 0x210C; // RB_STENCILREFMASK_BF
constexpr u32 kRegStencilRefMask = 0x210D;   // RB_STENCILREFMASK
constexpr u32 kRegAlphaRef = 0x210E;         // RB_ALPHA_REF (float)
constexpr u32 kRegDepthControl = 0x2200;     // RB_DEPTHCONTROL
constexpr u32 kRegBlendControl0 = 0x2201;    // RB_BLENDCONTROL0
constexpr u32 kRegColorControl = 0x2202;     // RB_COLORCONTROL
constexpr u32 kRegModeControl = 0x2205;      // PA_SU_SC_MODE_CNTL
// RB_BLENDCONTROL with src = ONE, dest = ZERO, ADD for color and alpha.
constexpr u32 kBlendControlNoBlend = 0x00010001u;
// Xenos CompareFunction::kAlways.
constexpr u32 kCompareAlways = 7;

plume::RenderCullMode ReadCullMode(u32 mode_control) {
  const bool cull_front = mode_control & 0x1u;
  const bool cull_back = mode_control & 0x2u;
  const bool front_cw = mode_control & 0x4u;  // face: 0 = front is CCW
  if (cull_front == cull_back)
    return plume::RenderCullMode::NONE;  // both set culls everything; not modeled
  // The pipeline's front face is CLOCKWISE (D3D convention), so culling the
  // clockwise winding is FRONT.
  const bool culled_cw = cull_front ? front_cw : !front_cw;
  return culled_cw ? plume::RenderCullMode::FRONT : plume::RenderCullMode::BACK;
}

plume::RenderFillMode ReadFillMode(u32 mode_control) {
  const u32 poly_mode = (mode_control >> 3) & 0x3u;
  const u32 front_type = (mode_control >> 5) & 0x7u;  // 0 points, 1 lines, 2 triangles
  return (poly_mode != 0 && front_type == 1) ? plume::RenderFillMode::WIREFRAME
                                             : plume::RenderFillMode::SOLID;
}

void ReadDeviceRenderState(VideoState &s, u32 device_guest) {
  const auto *dev = mgrn::mem::at<const D3DDevice>(device_guest);
  if (!dev)
    return;
  const u32 blend = ReadDeviceRegister(*dev, kRegBlendControl0, kBlendControlNoBlend);
  const u32 depth = ReadDeviceRegister(*dev, kRegDepthControl);
  const u32 color_control = ReadDeviceRegister(*dev, kRegColorControl);
  const u32 mode = ReadDeviceRegister(*dev, kRegModeControl);
  const u32 ref_mask = ReadDeviceRegister(*dev, kRegStencilRefMask, 0x00FFFF00u);
  const u32 ref_mask_bf = ReadDeviceRegister(*dev, kRegStencilRefMaskBf, ref_mask);

  bool &dirty = s.dirtyStates.pipelineState;
  PipelineState &ps = s.pipelineState;

  Video::SetDirtyValue(dirty, ps.alphaBlendEnable, blend != kBlendControlNoBlend);
  Video::SetDirtyValue(dirty, ps.srcBlend, ConvertBlendMode(blend & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOp, ConvertBlendOp((blend >> 5) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlend, ConvertBlendMode((blend >> 8) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.srcBlendAlpha, ConvertBlendMode((blend >> 16) & 0x1Fu));
  Video::SetDirtyValue(dirty, ps.blendOpAlpha, ConvertBlendOp((blend >> 21) & 0x7u));
  Video::SetDirtyValue(dirty, ps.destBlendAlpha, ConvertBlendMode((blend >> 24) & 0x1Fu));

  Video::SetDirtyValue(dirty, ps.zEnable, (depth & 0x2u) != 0);
  Video::SetDirtyValue(dirty, ps.zWriteEnable, (depth & 0x4u) != 0);
  Video::SetDirtyValue(dirty, ps.zFunc, ConvertCompareFunc((depth >> 4) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilEnable, (depth & 0x1u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilTwoSided, (depth & 0x80u) != 0);
  Video::SetDirtyValue(dirty, ps.stencilFunc, ConvertCompareFunc((depth >> 8) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFail, ConvertStencilOp((depth >> 11) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilPass, ConvertStencilOp((depth >> 14) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilZFail, ConvertStencilOp((depth >> 17) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFuncCCW, ConvertCompareFunc((depth >> 20) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilFailCCW, ConvertStencilOp((depth >> 23) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilPassCCW, ConvertStencilOp((depth >> 26) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilZFailCCW, ConvertStencilOp((depth >> 29) & 0x7u));
  Video::SetDirtyValue(dirty, ps.stencilRef, static_cast<u8>(ref_mask & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilMask, static_cast<u8>((ref_mask >> 8) & 0xFFu));
  Video::SetDirtyValue(dirty, ps.stencilWriteMask, static_cast<u8>((ref_mask >> 16) & 0xFFu));
  (void)ref_mask_bf;  // plume takes one ref/mask pair for both faces

  Video::SetDirtyValue(dirty, ps.cullMode, ReadCullMode(mode));
  Video::SetDirtyValue(dirty, ps.fillMode, ReadFillMode(mode));
  Video::SetDirtyValue(dirty, ps.colorWriteEnable,
                       ReadDeviceRegister(*dev, kRegColorMask, 0xFu) & 0xFu);

  // Alpha test is fixed-function on Xenos; the recompiled PS clips on the spec
  // constant against g_AlphaThreshold ("pass if alpha >= ref").
  const bool alpha_test =
      (color_control & 0x8u) != 0 && (color_control & 0x7u) != kCompareAlways;
  Video::SetAlphaTestMode(alpha_test);
  if (alpha_test) {
    const u32 ref_bits = ReadDeviceRegister(*dev, kRegAlphaRef);
    float ref;
    std::memcpy(&ref, &ref_bits, sizeof(ref));
    Video::SetAlphaThreshold(ref);
  }
}
} // namespace

bool Video::FlushRenderState(u32 device_guest) {
  std::lock_guard lock(state().mutex);
  return FlushRenderStateLocked(device_guest);
}

bool Video::FlushRenderStateLocked(u32 device_guest) {
  auto &s = state();
  // A confirmed device-removed event is terminal: stop recording so the render
  // thread cannot race the fatal dialog into a crash.
  if (DeviceIsLost())
    return false;
  if (!s.command_list_open)
    return false;
  if (!s.draw_framebuffer_bound)
    return false;
  // CPU zone: a GPU zone here would add two GPU timestamps per draw.
  MGRN_CPU_ZONE("FlushRenderState");

  // Fold the Set*-hook mirrors into the pipelineState the PSO lookup reads.
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexShader,
                s.vertex_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.pixelShader,
                s.pixel_shader);
  SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexDeclaration,
                s.vertex_declaration);

  for (u32 i = 0; i < 16; ++i) {
    SetDirtyValue(s.dirtyStates.pipelineState, s.pipelineState.vertexStrides[i],
                  static_cast<u8>(s.input_slots[i].stride));
  }

  // The PSO's formats must match the framebuffer BindDrawFramebuffer attached,
  // so they come from the same ResolveEffectiveTargets pair it binds.
  {
    GuestTexture *rt = nullptr;
    GuestTexture *ds = nullptr;
    ResolveEffectiveTargets(s, rt, ds);
    const auto rt_format = rt ? rt->format : plume::RenderFormat::UNKNOWN;
    const auto ds_format = ds ? ds->format : plume::RenderFormat::UNKNOWN;
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.renderTargetFormat, rt_format);
    SetDirtyValue(s.dirtyStates.pipelineState,
                  s.pipelineState.depthStencilFormat, ds_format);
  }

  // RB_DEPTHCONTROL + RB_BLENDCONTROL0 + RB_STENCILREFMASK + the color control
  // alpha enable bit. BD inlines its render state writes at LTCG-eligible sites
  // and routes the rest through bdSetRenderState's dispatch table, so the
  // register shadow merges both.
  ReadDeviceRenderState(s, device_guest);

  // Anything missing here means the engine has not wired the pipeline up yet.
  if (!s.pipelineState.vertexShader || !s.pipelineState.vertexDeclaration) {
    u32 n;
    if (DiagShouldLog(3, s.render_target, &n)) {
      MGRN_DEV_WARN("[draw-diag] #{} draw dropped: vs={} decl={} ps={} rt={}x{}", n,
             static_cast<void *>(s.pipelineState.vertexShader),
             static_cast<void *>(s.pipelineState.vertexDeclaration),
             static_cast<void *>(s.pipelineState.pixelShader),
             s.render_target ? s.render_target->width : 0,
             s.render_target ? s.render_target->height : 0);
    }
    return false;
  }

  // D3D12 retains the bound pipeline across draws in a command list, so a clean
  // pipelineState can skip both the cache lookup and the bind.
  // BeginCommandList force-dirties this on every command list reset.
  if (s.dirtyStates.pipelineState) {
    PipelineState lookup = s.pipelineState;
    SanitizePipelineState(lookup);
    bool built = false;
    auto *pso = GetOrCreatePipeline(lookup, &built);
    if (!pso) {
      u32 n;
      if (DiagShouldLog(4, s.render_target, &n)) {
        MGRN_DEV_WARN("[draw-diag] #{} draw dropped: PSO build failed (vs={} ps={} "
               "rt={}x{} fmt={})",
               n, static_cast<void *>(s.pipelineState.vertexShader),
               static_cast<void *>(s.pipelineState.pixelShader),
               s.render_target ? s.render_target->width : 0,
               s.render_target ? s.render_target->height : 0,
               u32(s.pipelineState.renderTargetFormat));
      }
      return false;
    }
    // 'built' means this draw compiled the PSO synchronously, so neither
    // residual nor predictor covered it. Warns once per pipeline, and
    // REBLUE_PSO_CAP builds also capture it for the residual/template tooling.
    RecordPipelineState(lookup, CurrentRenderPassId(), built);
    s.command_list->setPipeline(pso);
    s.current_pso = pso;
  } else if (!s.current_pso) {
    // Clean dirty bits but no PSO bound: the first draw after a command list
    // reset that lost the force-dirty.
    return false;
  }

  // The Set*ShaderConstant wrappers dirty-track these, so clean means the bound
  // constants are still live and the 4 KiB byte swap upload can be skipped.
  // Vulkan push offsets 0/8/16 follow the guest PushConstants member order
  // emitted by the recompiler.
  if (device_guest) {
    if (s.dirtyStates.vertexShaderConstants) {
      auto vs_alloc = UploadVertexShaderConstants(device_guest);
      if (vs_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(vs_alloc.ref, 0);
#else
        s.command_list->setGraphicsPushConstants(
            kGuestPushConstantRangeIndex, &vs_alloc.gpuAddress, 0, sizeof(u64));
#endif
      }
    }

    if (s.dirtyStates.pixelShaderConstants) {
      auto ps_alloc = UploadPixelShaderConstants(device_guest);
      if (ps_alloc.size) {
#if defined(REBLUE_D3D12)
        s.command_list->setGraphicsRootDescriptor(ps_alloc.ref, 1);
#else
        s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                                 &ps_alloc.gpuAddress,
                                                 sizeof(u64), sizeof(u64));
#endif
      }
    }

    // SharedConstants rebuilds from live guest state every draw: the sampler
    // fetch constants are written by unhooked recompiled code, so there is no
    // dirty signal. The upload is skipped internally when the built block is
    // byte-identical to the one already bound on this list.
    auto sc_alloc = UploadSharedConstants(device_guest);
    if (sc_alloc.size) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(sc_alloc.ref, 2);
#else
      s.command_list->setGraphicsPushConstants(kGuestPushConstantRangeIndex,
                                               &sc_alloc.gpuAddress,
                                               2 * sizeof(u64), sizeof(u64));
#endif
    }
  }

  // Lens flare occlusion count: the counter UAV (root descriptor 3 on D3D12,
  // set 4 on Vulkan) for the sun test quad draw bracketed by D3DQuery_Issue
  // BEGIN/END. The pipeline cache swaps occlusion_count_ps in while counting.
  if (s.occlusion_counting) {
    const u32 slot = s.frame.load(std::memory_order_relaxed);
    if (s.occlusion_counter[slot]) {
#if defined(REBLUE_D3D12)
      s.command_list->setGraphicsRootDescriptor(
          s.occlusion_counter[slot]->at(0), 3);
#else
      s.command_list->setGraphicsDescriptorSet(
          s.occlusion_descriptor_set[slot].get(), kOcclusionDescriptorSetIndex);
#endif
    }
  }

  // Clean state is first=255, last=0, so 'first <= last' skips the call when
  // nothing changed. BeginCommandList force-dirties the full range every
  // command list reset: D3D12 IA bindings do not survive begin().
  if (s.dirtyStates.vertexStreamFirst <= s.dirtyStates.vertexStreamLast) {
    const u32 first = s.dirtyStates.vertexStreamFirst;
    const u32 count = u32{s.dirtyStates.vertexStreamLast} - first + 1u;
    s.command_list->setVertexBuffers(first, s.vertex_views + first, count,
                                     s.input_slots + first);
  }

  // Re-binds only when SetIndices changed buffer/size/format, or
  // BeginCommandList force-dirtied after a command list reset.
  if (s.dirtyStates.indices && s.index_view.buffer.ref != nullptr) {
    s.command_list->setIndexBuffer(&s.index_view);
  }

  s.dirtyStates = DirtyStates(false);
  return true;
}

} // namespace mgrn::gpu
