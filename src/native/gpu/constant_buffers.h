/**
 * @file    gpu/constant_buffers.h
 * @brief   Shader constant upload path.
 *
 *   VS f4 constants at device+0x700, PS f4 at device+0x1700, SharedConstants
 *   bound through the main RenderPipelineLayout's three root descriptors
 *   (b0/b1/b2, space4).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

#include <plume_render_interface.h>

namespace mgrn::gpu {

// Mirror of the shared constant block the MGR_NATIVE_PIPELINE shaders read
// through g_PushConstants.SharedConstants (see shader_common.h). MGR layout:
// 32 fetch slots (Xenos vertex samplers start at 16), so each descriptor index
// table spans 128 bytes and the booleans start at 512. Reordering or resizing
// means regenerating the native shader cache.
constexpr u32 kFetchSlotCount = 32;
struct SharedConstants {
  u32 texture2DIndices[kFetchSlotCount]{};   // bytes 0..127
  u32 texture3DIndices[kFetchSlotCount]{};   // bytes 128..255
  u32 textureCubeIndices[kFetchSlotCount]{}; // bytes 256..383
  u32 samplerIndices[kFetchSlotCount]{};     // bytes 384..511
  u32 booleansArr[8]{};                      // bytes 512..543 (VS 0..127, PS 128..255)
  u32 swappedTexcoords{};                    // byte 544
  float halfPixelOffsetX{};                  // byte 548
  float halfPixelOffsetY{};                  // byte 552
  float alphaThreshold{};                    // byte 556
  u32 swappedNormals{};                      // byte 560
  u32 swappedBinormals{};                    // byte 564
  u32 swappedTangents{};                     // byte 568
  u32 swappedBlendWeights{};                 // byte 572
  u32 swappedPositions{};                    // byte 576
  // Bit N set when TEXCOORD<N> is bound R16G16(B16A16)_UINT, and the shader's
  // sintTexcoord() sign-extends to recover the X360 raw-int-as-float vfetch.
  u32 sintTexcoords{}; // byte 580
  // Shadow PCF tap scale (g_ShadowPcfScale), see RecomputeShadowPcfScale.
  float shadowPcfScale{1.0f}; // byte 584
  u32 _pad588{};              // byte 588
  // Half-pixel NDC nudge read only by host blit shaders.
  float blitHalfPixelOffsetX{}; // byte 592
  float blitHalfPixelOffsetY{}; // byte 596
  u32 _pad600[2]{};             // bytes 600..607
};
static_assert(offsetof(SharedConstants, samplerIndices) == 384);
static_assert(offsetof(SharedConstants, booleansArr) == 512);
static_assert(offsetof(SharedConstants, swappedTexcoords) == 544);
static_assert(offsetof(SharedConstants, alphaThreshold) == 556);
static_assert(offsetof(SharedConstants, shadowPcfScale) == 584);
static_assert(sizeof(SharedConstants) == 608);

// memory stays valid until ResetFrame().
struct ConstantAllocation {
  u8 *memory = nullptr;
  plume::RenderBufferReference ref{};
  u64 gpuAddress = 0;
  u32 size = 0;
};

bool TryInit();

// Rewind a frame slot's upload chunks. Caller must have awaited that slot's
// GPU fence
// (DrainSlot), never while in flight.
void ResetFrame(u32 slot);

// Byte-swap VS/PS float constants from guest device+0x700 / device+0x1700 into
// the upload heap.
ConstantAllocation UploadVertexShaderConstants(u32 device_guest);
ConstantAllocation UploadPixelShaderConstants(u32 device_guest);

// Rebuilt from live guest state every draw: sampler fetch constants and bool
// constants come from unhooked recompiled code, so there is no dirty signal.
// size == 0 means byte-identical to what is already bound on this command list
// and the caller can skip the upload + root rebind.
ConstantAllocation UploadSharedConstants(u32 device_guest);

// Root bindings do not survive begin(), so this drops the 'shared CB still
// bound' reuse gate. Call on every command list reset.
void InvalidateSharedBinding();

// alignment defaults to 4 (the upload buffer is WRITE_COMBINE), and CBV callers
// pass 256.
ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment = 4);

// Verbatim, no swap, for texture pixel data.
ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment = 4);

// For pixel data already untiled to host staging.
ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment = 4);

} // namespace mgrn::gpu
