// MGR's statically linked XDK (July 2012) D3D entry points, named for the
// native renderer. Each alias maps an XDK name to the recompiled guest symbol,
// so REX_HOOK(D3DDevice_X, ...) and __imp__D3DDevice_X resolve to sub_<addr>.
//
// Evidence key: R = per-frame call count from scripts/d3d-census.sh matches
// the role, S = recompiled body read (stores, packets, callees). Counts are a
// gameplay window (private/d3d/census-xenos1.5400.tsv); device = 0x40004180,
// pointer kept by the engine in the global at kEngineDevicePointer.
#pragma once

#include <cstdint>

namespace mgr::native {

constexpr uint32_t kEngineDevicePointer = 0x83733A08;  // lwz r3,14856(0x83730000) in 8278DFC8

}  // namespace mgr::native

// --- Draws --------------------------------------------------------------------
// R 634/frame, S: flushes dirty state blocks, IM_LOAD via 826CB580, DRAW_INDX.
// (device, D3DPRIMITIVETYPE, BaseVertexIndex, StartIndex, IndexCount); the
// engine wrapper 8278DFC8 turns a primitive count into IndexCount.
#define D3DDevice_DrawIndexedVertices sub_826BCD40
// R 98/frame, S: same flush, DRAW_INDX auto-index. (device, type, StartVertex, VertexCount).
#define D3DDevice_DrawVertices sub_826BC928

// --- Render targets, resolve, clear ------------------------------------------
// R 21/frame, S: stores surface at device+0x3210+idx*4. (device, index, surface).
#define D3DDevice_SetRenderTarget sub_826B8ED8
#define D3DDevice_SetRenderTarget_Thunk sub_826B9540  // b 0x826B8ED8
// R 4/frame, S: stores surface at device+0x3220. (device, surface).
#define D3DDevice_SetDepthStencilSurface sub_826B9268
// S: loads device+0x3210+idx*4 and AddRefs it (826AA640).
#define D3DDevice_GetRenderTarget sub_826B8820
// R 21/frame (= Xenos resolves), S: wrapper of 826B9F78 (RB_COPY_* registers).
#define D3DDevice_Resolve sub_826BB0D0
#define D3D_ResolveBody sub_826B9F78
// R 6/frame from the engine, S: per-rect loop calling 826BC260 with color/z/stencil.
#define D3DDevice_Clear sub_826BC398

// --- Viewport, scissor ---------------------------------------------------------
// R 33/frame, S: D3DVIEWPORT9 ints to floats, then 826B8B40 (device+0x32E0..).
#define D3DDevice_SetViewport sub_826B8DA8
// S: scissor rect stored at device+0x32FC..0x3308; 826B8D98 toggles device+0x3000.
#define D3DDevice_SetScissorRect sub_826B8578

// --- Shaders, constants, vertex input -----------------------------------------
// R 280/frame, S: device+0x3310. Runtime-verified: the object it binds is a
// vertex shader container (a first guess had the two setters swapped).
#define D3DDevice_SetVertexShader sub_826AC618
// R 280/frame, S: device+0x330C, binds pixel shader containers.
#define D3DDevice_SetPixelShader sub_826AC410
// R 853 at load (PS count), S: takes the shader container (flags, virtual, physical size).
#define D3DDevice_CreatePixelShader sub_826ACC10
// R 396 at load (VS count), S: same container parsing, 872-byte object.
#define D3DDevice_CreateVertexShader sub_826ACDF8
// R 1460/frame, S: copies to device+0x780+start*16, ORs dirty mask into device+0.
#define D3DDevice_SetVertexShaderConstantF sub_826AC0B8
// R 480/frame, S: device+0x1780+start*16.
#define D3DDevice_SetPixelShaderConstantF sub_826AC190
// R 508/frame, S: stw r4,0x2FD0(r3), dirty bit 0x80000 in device+16.
#define D3DDevice_SetVertexDeclaration sub_826AC830
// R 77 at load, S: walks 12-byte D3DVERTEXELEMENT9 to the 0xFF end marker.
#define D3DDevice_CreateVertexDeclaration sub_826AC938
// R 642/frame, S: writes the vertex fetch constant and device+0x3224+stream*4.
#define D3DDevice_SetStreamSource sub_826B8670
// R 430/frame, S: stores the index buffer at device+0x320C.
#define D3DDevice_SetIndices sub_826B8790

// --- Textures, surfaces -------------------------------------------------------
// R 477/frame, S: copies the texture fetch constant (+0x1C) to device+0x480+sampler*24.
#define D3DDevice_SetTexture sub_826AEFD0
// R 56 at load, S: allocates 52 bytes (sizeof(D3DTexture)).
#define D3DDevice_CreateTexture sub_826AEC50
// R 131 at load, S: allocates 48 bytes (sizeof(D3DSurface)), checks depth formats.
#define D3DDevice_CreateSurface sub_826AED70
// S: resource type 3/17/18/19/20 switch; texture header setup (XGSetTextureHeader-like).
#define D3D_SetTextureHeader sub_826AE488
// S: returns ((fetch dword at +44 >> 6) & 15) + 1.
#define D3DBaseTexture_GetLevelCount sub_826AE9F0

// --- Present and timing (kept running on the guest side) -----------------------
// R 1/frame from the engine present 82791828; calls VdSwap.
#define D3DDevice_Swap sub_826B3510
// R 1/frame from 82791828: restores default RT/DS/shaders before present.
#define D3D_RestoreDefaultState sub_826B20E0
// R 1.7M/frame from the engine busy-wait 8278E640 (GPU idle poll).
#define D3DDevice_PollGpuIdle sub_826B2FC0

// --- Internal state flush (called by the draws) --------------------------------
// (device, mask, register base, source) for contiguous register blocks.
#define D3D_FlushRegisterBlock sub_826CA450
// ALU constants (VS 0x4000 from +0x780, PS 0x4400 from +0x1780).
#define D3D_FlushAluConstants sub_826CA7E8
#define D3D_FlushFetchConstants sub_826CA690
// Shader microcode load (IM_LOAD) and SQ_PROGRAM_CNTL.
#define D3D_FlushShaders sub_826CB580
