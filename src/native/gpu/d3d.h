/**
 * @file    gpu/d3d.h
 * @brief   Byte-accurate guest layouts for the guest D3D9 SDK objects the
 *          recompiled BD engine reads/writes.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 *
 * Must match the guest layout byte-for-byte at the offsets the engine
 * accesses, or accessors like D3DTexture_GetLevelDesc decode garbage and
 * crash D3D12. Layouts recovered from the guest binary (D3DResource ord 933,
 * D3DTexture ord 2657, D3DSurface ord 2525, D3DVertexBuffer ord 1936,
 * D3DIndexBuffer ord 1937, D3DVertexShader ord 1641, D3DPixelShader ord 1263,
 * D3DVertexDeclaration ord 934, GPUTEXTURE_FETCH_CONSTANT ord 853). Only
 * host-accessed fields are named.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

namespace mgrn::gpu {

// D3DRESOURCETYPE values from the guest D3D9 SDK. D3DResource::Common carries
// the type in its low 4 bits, so these double as the Common base type bits.
enum class D3DResourceType : u32 {
  kSurface = 1,
  kTexture = 3,
  kVolumeTexture = 4,
  kCubeTexture = 5,
  kVertexBuffer = 6,
  kIndexBuffer = 7,
};

// 24-byte header shared by every D3D9 resource (D3DResource ord 933).
struct D3DResource {
  be_u32 Common;         // +0x00  type/flag bits, read by D3DResource_GetType
  be_u32 ReferenceCount; // +0x04  engine increments/decrements directly
  be_u32 Fence;          // +0x08
  be_u32 ReadFence;      // +0x0C
  be_u32 Identifier;     // +0x10
  be_u32 BaseFlush;      // +0x14
};
static_assert(sizeof(D3DResource) == 24);

// The pViewport argument to D3DDevice_SetViewport (BD 0x82473610), and the
// layout of the device's cached viewport at D3DDevice+0x3058.
struct D3DViewport9 {
  be_u32 X;
  be_u32 Y;
  be_u32 Width;
  be_u32 Height;
  be_f32 MinZ;
  be_f32 MaxZ;
};
static_assert(sizeof(D3DViewport9) == 24);

// Guest RECT (LTRB): the pRect argument to D3DDevice_SetScissorRect
// (BD 0x82473548).
struct D3DRect {
  be_i32 left;
  be_i32 top;
  be_i32 right;
  be_i32 bottom;
};
static_assert(sizeof(D3DRect) == 16);

// GPUTEXTURE_FETCH_CONSTANT (ord 853): 6-dword Xenos texture descriptor at
// D3DTexture+0x1C. reblue-created textures leave it zero since their accessors
// are hooked. Engine-created bdAllocRenderBuffer textures carry a real one that
// native_texture_mirror decodes.
struct GpuTextureFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(GpuTextureFetchConstant) == 24);

// D3DTexture / D3DBaseTexture / D3DCubeTexture / D3DVolumeTexture, 52 bytes.
struct D3DTexture {
  D3DResource resource;           // +0x00
  be_u32 MipFlush;                // +0x18
  GpuTextureFetchConstant Format; // +0x1C
};
static_assert(sizeof(D3DTexture) == 52);
static_assert(offsetof(D3DTexture, Format) == 0x1C);

// D3DSurface, 48 bytes. D3DSurface_GetDesc (0x8246D668, non-texture path)
// unpacks Width/Height from the dword at +0x24:
//   Width  = (SizeBits >> 18) + 1            (width-1 in bits [31:18])
//   Height = ((SizeBits >> 3) & 0x7FFF) + 1  (height-1 in bits [17:3])
// Bits [2:0] are pad.
struct D3DSurface {
  D3DResource resource; // +0x00
  be_u32 SurfaceInfo;   // +0x18  GPU_SURFACEINFO union word
  be_u32 DepthInfo;     // +0x1C  GPU_DEPTHINFO union word
  be_u32 HiControl;     // +0x20
  be_u32 SizeBits;      // +0x24  packed Height/Width
  be_u32 Format;        // +0x28  D3DFORMAT
  be_u32 Size;          // +0x2C
};
static_assert(sizeof(D3DSurface) == 48);

// Verified against the code at 0x8246D668. Written by D3DSurface_GetDesc
// and D3DTexture_GetLevelDesc.
struct D3DSurfaceDesc {
  be_u32 Format;             // +0x00  guest D3DFORMAT
  be_u32 Type;               // +0x04  D3DRESOURCETYPE
  be_u32 Usage;              // +0x08  always written 0 by reblue
  be_u32 Pool;               // +0x0C  always written 0 by reblue
  be_u32 MultiSampleType;    // +0x10  0, since reblue surfaces are COUNT_1
  be_u32 MultiSampleQuality; // +0x14  0
  be_u32 Width;              // +0x18  surface / mip-level width
  be_u32 Height;             // +0x1C  surface / mip-level height
};
static_assert(sizeof(D3DSurfaceDesc) == 32);

// Written by D3DVolumeTexture_GetLevelDesc. Adds Depth and drops the
// MultiSample fields, so NOT the D3DSurfaceDesc layout.
struct D3DVolumeDesc {
  be_u32 Format; // +0x00  guest D3DFORMAT
  be_u32 Type;   // +0x04  4 == D3DRTYPE_VOLUMETEXTURE
  be_u32 Usage;  // +0x08  0
  be_u32 Pool;   // +0x0C  0
  be_u32 Width;  // +0x10  mip-level width
  be_u32 Height; // +0x14  mip-level height
  be_u32 Depth;  // +0x18  mip-level depth
};
static_assert(sizeof(D3DVolumeDesc) == 28);

// D3DLOCKED_RECT. Points the engine at the texture's guest scratch mirror.
struct D3DLockedRect {
  be_u32 Pitch; // +0x00  row pitch in bytes
  be_u32 pBits; // +0x04  guest VA of the locked pixel scratch
};
static_assert(sizeof(D3DLockedRect) == 8);

// D3DVertexBuffer / D3DIndexBuffer, 32 bytes. The +0x18/+0x1C pair is the Xenos
// fetch constant, encoded two ways depending on who wrote it:
//   Pre-XGOffset (bdPhysical*Create replication, raw dwords):
//     VB writes size@FetchLo/base@FetchHi, IB writes (size|3)@FetchLo/
//     ((base & 0x3FFFFFC)|0x10000002)@FetchHi.
//   Post-XGOffsetResourceAddress (asset-loaded structs, Bootstrap path):
//     FetchLo = base_va | tag_bits, FetchHi = (size & 0x3FFFFFC) | 0x10000002.
// reblue's own mirror and bootstrap paths use the post-XGOffset encoding.
struct D3DVertexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18
  be_u32 FetchHi; // +0x1C
};
static_assert(sizeof(D3DVertexBuffer) == 32);
static_assert(offsetof(D3DVertexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DVertexBuffer, FetchHi) == 0x1C);

struct D3DIndexBuffer {
  D3DResource resource;
  be_u32 FetchLo; // +0x18
  be_u32 FetchHi; // +0x1C
};
static_assert(sizeof(D3DIndexBuffer) == 32);
static_assert(offsetof(D3DIndexBuffer, FetchLo) == 0x18);
static_assert(offsetof(D3DIndexBuffer, FetchHi) == 0x1C);

// D3DVertexShader / D3DPixelShader / D3DVertexDeclaration, 24 bytes each.
struct D3DVertexShader {
  D3DResource resource;
};
struct D3DPixelShader {
  D3DResource resource;
};
struct D3DVertexDeclaration {
  D3DResource resource;
};
static_assert(sizeof(D3DVertexShader) == 24);
static_assert(sizeof(D3DPixelShader) == 24);
static_assert(sizeof(D3DVertexDeclaration) == 24);

// The microcode blob CreateVertexShader/CreatePixelShader are handed, layout
// mirrors thirdparty/XenosRecomp/XenosRecomp/shader.h.
struct ShaderContainer {
  be_u32 flags;
  be_u32 virtualSize;
  be_u32 physicalSize;
  be_u32 fieldC;
  be_u32 constantTableOffset;
  be_u32 definitionTableOffset;
  be_u32 shaderOffset;
  be_u32 field1C;
  be_u32 field20;
};
static_assert(sizeof(ShaderContainer) == 0x24);


// dword[0] bits [1:0] of a fetch constant tag what it describes.
constexpr u32 kFetchTypeMask = 0x3;
constexpr u32 kFetchTypeTexture = 2;
constexpr u32 kFetchTypeVertex = 3;

// 24-byte Xenos GPU fetch descriptor. D3DDevice holds 32 in fetchConstants[].
struct DeviceFetchConstant {
  be_u32 dword[6];
};
static_assert(sizeof(DeviceFetchConstant) == 24);

// MGR's guest D3DDevice (July 2012 XDK, allocated by the guest's own
// Direct3D_CreateDevice at 0x40004180). Offsets come from the recompiled setters
// and the draw-time state flush (src/native/d3d_functions.h); only fields the
// native renderer reads are named. The XDK keeps GPU register images in
// contiguous blocks (kRegisterBlocks), which the draw path decodes with Xenos
// register semantics.
struct D3DDevice {
  // m_Pending dirty masks: [0] VS constants, [1] PS constants, [2] state,
  // [3] fetch constants/samplers, [4] misc. Read and cleared by the guest draws.
  be_u64 m_Mask[5];                       // +0x000
  u8 pad_0028[0x480 - 0x028];             // +0x028
  // 32 unified fetch slots: texture (type 2) or three vertex fetches (type 3).
  // SetStreamSource(stream) writes vertex fetch constant 95 - stream.
  DeviceFetchConstant fetchConstants[32]; // +0x480
  be_f32 vsFloatConstants[256][4];        // +0x780  (registers 0x4000..)
  be_f32 psFloatConstants[256][4];        // +0x1780 (registers 0x4400..)
  // SHADER_CONSTANT_BOOL_000_031..224_255 (register 0x4900): VS 0..127, PS 128..255.
  be_u32 vsBoolConstants[4];              // +0x2780
  be_u32 psBoolConstants[4];              // +0x2790
  u8 pad_27A0[0x2880 - 0x27A0];           // +0x27A0  loop constants, packets
  be_u32 regs2000[19];                    // +0x2880  0x2000..0x2012 (RB_SURFACE_INFO..)
  be_u32 regs2100[21];                    // +0x28CC  0x2100..0x2114 (PA_CL_VPORT_* at 0x210F)
  be_u32 regs2180[5];                     // +0x2920  0x2180..0x2184 (SQ_PROGRAM_CNTL..)
  be_u32 regs2200[12];                    // +0x2934  0x2200..0x220B (RB_DEPTHCONTROL..)
  be_u32 regs2280[21];                    // +0x2964  0x2280..0x2294
  be_u32 regs2300[38];                    // +0x29B8  0x2300..0x2325 (RB_COPY_*, clears)
  u8 pad_2A50[0x2FD0 - 0x2A50];           // +0x2A50  0x2380 block and the rest
  be_u32 vertexDeclaration;               // +0x2FD0  D3DDevice_SetVertexDeclaration
  u8 pad_2FD4[0x320C - 0x2FD4];           // +0x2FD4
  be_u32 indexBuffer;                     // +0x320C  D3DDevice_SetIndices
  be_u32 renderTargetShadow[4];           // +0x3210  D3DDevice_SetRenderTarget
  be_u32 depthStencilShadow;              // +0x3220  D3DDevice_SetDepthStencilSurface
  be_u32 streamSources[16];               // +0x3224  D3DDevice_SetStreamSource
  u8 pad_3264[0x3278 - 0x3264];           // +0x3264
  be_u32 textures[26];                    // +0x3278  D3DDevice_SetTexture (sampler)
  u8 pad_32E0[0x32E0 - 0x32E0];           // +0x32E0
  // D3DDevice_SetViewport (826B8B40) copy: X, Y, Width, Height, MinZ, MaxZ.
  be_f32 viewport[6];                     // +0x32E0
  u8 pad_32F8[0x32FC - 0x32F8];           // +0x32F8
  D3DRect scissorRect;                    // +0x32FC  D3DDevice_SetScissorRect
  u8 pad_330C[0x330C - 0x330C];           // +0x330C
  be_u32 pixelShader;                     // +0x330C  D3DDevice_SetPixelShader
  be_u32 vertexShader;                    // +0x3310  D3DDevice_SetVertexShader
};
static_assert(offsetof(D3DDevice, fetchConstants) == 0x480);
static_assert(offsetof(D3DDevice, vsFloatConstants) == 0x780);
static_assert(offsetof(D3DDevice, psFloatConstants) == 0x1780);
static_assert(offsetof(D3DDevice, vsBoolConstants) == 0x2780);
static_assert(offsetof(D3DDevice, regs2000) == 0x2880);
static_assert(offsetof(D3DDevice, regs2100) == 0x28CC);
static_assert(offsetof(D3DDevice, regs2180) == 0x2920);
static_assert(offsetof(D3DDevice, regs2200) == 0x2934);
static_assert(offsetof(D3DDevice, regs2280) == 0x2964);
static_assert(offsetof(D3DDevice, regs2300) == 0x29B8);
static_assert(offsetof(D3DDevice, vertexDeclaration) == 0x2FD0);
static_assert(offsetof(D3DDevice, indexBuffer) == 0x320C);
static_assert(offsetof(D3DDevice, renderTargetShadow) == 0x3210);
static_assert(offsetof(D3DDevice, depthStencilShadow) == 0x3220);
static_assert(offsetof(D3DDevice, streamSources) == 0x3224);
static_assert(offsetof(D3DDevice, textures) == 0x3278);
static_assert(offsetof(D3DDevice, viewport) == 0x32E0);
static_assert(offsetof(D3DDevice, scissorRect) == 0x32FC);
static_assert(offsetof(D3DDevice, pixelShader) == 0x330C);
static_assert(offsetof(D3DDevice, vertexShader) == 0x3310);

// One contiguous GPU register image in the device (see D3DDevice).
struct RegisterBlock {
  u32 first_register;
  u32 count;
  u32 device_offset;
};
inline constexpr RegisterBlock kRegisterBlocks[] = {
    {0x2000, 19, 0x2880}, {0x2100, 21, 0x28CC}, {0x2180, 5, 0x2920},
    {0x2200, 12, 0x2934}, {0x2280, 21, 0x2964}, {0x2300, 38, 0x29B8},
    // PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET} only; the block runs on.
    {0x2380, 4, 0x2A50},
};

// Value of GPU register 'reg' as the guest last set it, or 'fallback' when the
// register is not shadowed in the device.
inline u32 ReadDeviceRegister(const D3DDevice &device, u32 reg, u32 fallback = 0) {
  for (const RegisterBlock &block : kRegisterBlocks) {
    if (reg >= block.first_register && reg < block.first_register + block.count) {
      const auto *words = reinterpret_cast<const be_u32 *>(
          reinterpret_cast<const u8 *>(&device) + block.device_offset);
      return words[reg - block.first_register];
    }
  }
  return fallback;
}

// D3DResource_GetType decodes the type from D3DResource:
//   base = Common & 0xF
//     1 -> Surface  (extended to 16 if Common & 0x40000000 + texture-backed)
//     3 -> Texture, then dim = (fetch_dword5 >> 9) & 3:
//            0 -> returns 20 (1D / line texture)
//            1 -> returns 3 if !tiled, 19 if tiled (fetch_dword1 & 0x400)
//            2 -> returns 17 (volume texture)
//            3 -> returns 18 (stacked cube)
//     6 -> VertexBuffer
//     7 -> IndexBuffer
// The Common low-4 bits select the base type. Texture dimensionality lives in
// the fetch constant. Bit 30 (0x40000000) on a surface marks it texture-backed,
// which routes D3DSurface_GetDesc through D3DTexture_GetLevelDesc.
constexpr u32 kCommonFlagTextureBackedSurface = 0x40000000u;

constexpr u32 kClearTarget = 0x1u;
constexpr u32 kClearZBuffer = 0x10u;
constexpr u32 kClearStencil = 0x20u;
constexpr u32 kClearAll = kClearTarget | kClearZBuffer | kClearStencil;

constexpr u32 kResolveDepthStencil = 0x4u;
constexpr u32 kResolveExponentBiasShift = 26;

inline void InitResourceHeader(D3DResource &r, D3DResourceType type) {
  r.Common = u32(type);
  r.ReferenceCount = 1u;
  r.Fence = 0u;
  r.ReadFence = 0u;
  r.Identifier = 0u;
  r.BaseFlush = 0u;
}

} // namespace mgrn::gpu
