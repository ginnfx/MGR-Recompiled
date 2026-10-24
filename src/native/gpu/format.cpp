/**
 * @file    gpu/format.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/format.h"

#include <atomic>

#include "core/logging.h"
#include "gpu/device.h"

namespace mgrn::gpu {
namespace {
namespace xe = rex::graphics::xenos;

void WarnUnmapped(const char *what, u32 value) {
  static std::atomic<u32> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    MGRN_WARN("{}: no plume mapping for value {}, using the neutral default",
            what, value);
  }
}

} // namespace

plume::RenderBlend ConvertBlendMode(u32 value) {
  using B = plume::RenderBlend;
  switch (static_cast<xe::BlendFactor>(value)) {
  case xe::BlendFactor::kZero:
    return B::ZERO;
  case xe::BlendFactor::kOne:
    return B::ONE;
  case xe::BlendFactor::kSrcColor:
    return B::SRC_COLOR;
  case xe::BlendFactor::kOneMinusSrcColor:
    return B::INV_SRC_COLOR;
  case xe::BlendFactor::kSrcAlpha:
    return B::SRC_ALPHA;
  case xe::BlendFactor::kOneMinusSrcAlpha:
    return B::INV_SRC_ALPHA;
  case xe::BlendFactor::kDstColor:
    return B::DEST_COLOR;
  case xe::BlendFactor::kOneMinusDstColor:
    return B::INV_DEST_COLOR;
  case xe::BlendFactor::kDstAlpha:
    return B::DEST_ALPHA;
  case xe::BlendFactor::kOneMinusDstAlpha:
    return B::INV_DEST_ALPHA;
  case xe::BlendFactor::kSrcAlphaSaturate:
    return B::SRC_ALPHA_SAT;
  // The constant-color/alpha factors need a blend constant nothing feeds.
  default:
    WarnUnmapped("ConvertBlendMode", value);
    return B::ZERO;
  }
}

plume::RenderBlendOperation ConvertBlendOp(u32 value) {
  using O = plume::RenderBlendOperation;
  switch (static_cast<xe::BlendOp>(value)) {
  case xe::BlendOp::kAdd:
    return O::ADD;
  case xe::BlendOp::kSubtract:
    return O::SUBTRACT;
  case xe::BlendOp::kMin:
    return O::MIN;
  case xe::BlendOp::kMax:
    return O::MAX;
  case xe::BlendOp::kRevSubtract:
    return O::REV_SUBTRACT;
  default:
    WarnUnmapped("ConvertBlendOp", value);
    return O::ADD;
  }
}

plume::RenderComparisonFunction ConvertCompareFunc(u32 value) {
  using F = plume::RenderComparisonFunction;
  switch (static_cast<xe::CompareFunction>(value)) {
  case xe::CompareFunction::kNever:
    return F::NEVER;
  case xe::CompareFunction::kLess:
    return F::LESS;
  case xe::CompareFunction::kEqual:
    return F::EQUAL;
  case xe::CompareFunction::kLessEqual:
    return F::LESS_EQUAL;
  case xe::CompareFunction::kGreater:
    return F::GREATER;
  case xe::CompareFunction::kNotEqual:
    return F::NOT_EQUAL;
  case xe::CompareFunction::kGreaterEqual:
    return F::GREATER_EQUAL;
  case xe::CompareFunction::kAlways:
    return F::ALWAYS;
  default:
    return F::LESS_EQUAL; // the D3D9 default
  }
}

plume::RenderStencilOp ConvertStencilOp(u32 value) {
  using S = plume::RenderStencilOp;
  switch (static_cast<xe::StencilOp>(value)) {
  case xe::StencilOp::kKeep:
    return S::KEEP;
  case xe::StencilOp::kZero:
    return S::ZERO;
  case xe::StencilOp::kReplace:
    return S::REPLACE;
  case xe::StencilOp::kIncrementClamp:
    return S::INCREMENT_AND_CLAMP;
  case xe::StencilOp::kDecrementClamp:
    return S::DECREMENT_AND_CLAMP;
  case xe::StencilOp::kInvert:
    return S::INVERT;
  case xe::StencilOp::kIncrementWrap:
    return S::INCREMENT_AND_WRAP;
  case xe::StencilOp::kDecrementWrap:
    return S::DECREMENT_AND_WRAP;
  default:
    return S::KEEP;
  }
}

// reblue keeps the D3D9 default front winding (frontFace=CLOCKWISE), so CW
// culls front and CCW culls back.
plume::RenderCullMode ConvertCullMode(u32 value) {
  switch (static_cast<D3DCull>(value)) {
  case D3DCull::kCW:
    return plume::RenderCullMode::FRONT;
  case D3DCull::kCCW:
    return plume::RenderCullMode::BACK;
  case D3DCull::kNone:
  default:
    return plume::RenderCullMode::NONE;
  }
}

plume::RenderFillMode ConvertFillMode(u32 value) {
  switch (static_cast<D3DFillMode>(value)) {
  case D3DFillMode::kWireframe:
    return plume::RenderFillMode::WIREFRAME;
  case D3DFillMode::kSolid:
  default:
    return plume::RenderFillMode::SOLID;
  }
}

// GPUTEXTUREFORMAT byte -> the plume format a mirror of the guest texels
// uploads as, block for block (no CPU conversion).
bool MirrorFormatFromGuestByte(u32 guest_format, plume::RenderFormat &out) {
  using TF = xe::TextureFormat;
  using F = plume::RenderFormat;
  switch (static_cast<TF>(guest_format)) {
  case TF::k_DXT1:
    out = F::BC1_UNORM;
    return true;
  case TF::k_DXT2_3:
    out = F::BC2_UNORM;
    return true;
  case TF::k_DXT4_5:
    out = F::BC3_UNORM;
    return true;
  case TF::k_DXT5A:
    out = F::BC4_UNORM;
    return true;
  case TF::k_DXN:
    out = F::BC5_UNORM;
    return true;
  case TF::k_8:
  case TF::k_8_A:
  case TF::k_8_B:
    out = F::R8_UNORM;
    return true;
  case TF::k_8_8:
    out = F::R8G8_UNORM;
    return true;
  case TF::k_8_8_8_8:
  case TF::k_8_8_8_8_A:
    out = F::R8G8B8A8_UNORM;
    return true;
  case TF::k_16:
    out = F::R16_UNORM;
    return true;
  case TF::k_16_16:
    out = F::R16G16_UNORM;
    return true;
  case TF::k_16_16_16_16:
    out = F::R16G16B16A16_UNORM;
    return true;
  case TF::k_16_FLOAT:
    out = F::R16_FLOAT;
    return true;
  case TF::k_16_16_FLOAT:
    out = F::R16G16_FLOAT;
    return true;
  case TF::k_16_16_16_16_FLOAT:
    out = F::R16G16B16A16_FLOAT;
    return true;
  case TF::k_32_FLOAT:
    out = F::R32_FLOAT;
    return true;
  case TF::k_32_32_FLOAT:
    out = F::R32G32_FLOAT;
    return true;
  case TF::k_32_32_32_32_FLOAT:
    out = F::R32G32B32A32_FLOAT;
    return true;
  default:
    return false;
  }
}

plume::RenderFormat ConvertGuestFormat(u32 guest_format) {
  switch (static_cast<D3DFormat>(guest_format)) {
  case D3DFormat::kA16B16G16R16F:
  case D3DFormat::kA16B16G16R16FAlt:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  case D3DFormat::kA8B8G8R8:
  case D3DFormat::kA8R8G8B8:
  case D3DFormat::kX8R8G8B8:
  case D3DFormat::kBD8888:
  case D3DFormat::kBD8888Alt:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DFormat::kBD2101010As16161616:
  case D3DFormat::kBare2101010As16161616:
    // X360 7e3 HDR EDRAM format. It must stay float: UNORM would clamp the
    // HDR scene the posteff chain relies on.
    return Video::SceneColorFormat();
  case D3DFormat::kD24FS8:
  case D3DFormat::kD24S8:
    return Video::DepthStencilFormat();
  case D3DFormat::kR32F:
    return plume::RenderFormat::R32_FLOAT;
  case D3DFormat::kG16R16F:
  case D3DFormat::kG16R16FAlt:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DFormat::kIndex16:
    return plume::RenderFormat::R16_UINT;
  case D3DFormat::kIndex32:
    return plume::RenderFormat::R32_UINT;
  case D3DFormat::kL8:
  case D3DFormat::kL8Alt:
  case D3DFormat::kA8:
    // A8 stores as single-channel R8_UNORM, and the shader swizzles at sample
    // time.
    return plume::RenderFormat::R8_UNORM;
  default:
    break;
  }
  // MGR: decode the D3DFORMAT fields instead of listing every variant.
  const plume::RenderFormat decoded = DecodeD3DFormat(guest_format);
  if (decoded == plume::RenderFormat::UNKNOWN) {
    // Never invent a format: callers make RT capability decisions off the
    // result. UNKNOWN forces a clean failure at the actual problem site.
    MGRN_ERROR(
        "ConvertGuestFormat: unknown guest format 0x{:08X}, returning UNKNOWN",
        guest_format);
  }
  return decoded;
}

// D3DFORMAT layout (Xbox 360 XDK): GPUTEXTUREFORMAT bits 0-5, endian 6-7,
// tiled 8, per-channel sign 9-16 (2 bits each X,Y,Z,W: 0 unsigned, 1 signed,
// 2 biased, 3 gamma), NUMFORMAT 17, swizzle 18-29. Channel order (swizzle)
// is not modeled, as in re:Blue's fixed table: render targets write and
// resolves copy in RGBA order.
plume::RenderFormat DecodeD3DFormat(u32 guest_format) {
  using TF = xe::TextureFormat;
  using F = plume::RenderFormat;
  const auto texture_format = static_cast<TF>(guest_format & 0x3Fu);
  const u32 signs = (guest_format >> 9) & 0xFFu;
  const bool gamma = (signs & 0x3u) == 3u || ((signs >> 2) & 0x3u) == 3u ||
                     ((signs >> 4) & 0x3u) == 3u;
  const bool is_signed = (signs & 0x3u) == 1u;
  switch (texture_format) {
  case TF::k_8:
  case TF::k_8_A:
  case TF::k_8_B:
    return F::R8_UNORM;
  case TF::k_8_8:
    return F::R8G8_UNORM;
  case TF::k_8_8_8_8:
  case TF::k_8_8_8_8_A:
    return gamma ? F::R8G8B8A8_UNORM_SRGB : is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
  case TF::k_2_10_10_10:
    return F::R16G16B16A16_UNORM;  // no 10:10:10:2 in plume; same precision or better
  case TF::k_2_10_10_10_AS_16_16_16_16:
  case TF::k_2_10_10_10_FLOAT_EDRAM:
    return Video::SceneColorFormat();  // X360 7e3 HDR EDRAM format
  case TF::k_8_8_8_8_AS_16_16_16_16:
    return F::R16G16B16A16_UNORM;
  case TF::k_8_8_8_8_GAMMA_EDRAM:
    return F::R8G8B8A8_UNORM_SRGB;
  case TF::k_16:
    return F::R16_UNORM;
  case TF::k_16_16:
    return F::R16G16_UNORM;
  case TF::k_16_16_16_16:
    return F::R16G16B16A16_UNORM;
  case TF::k_16_FLOAT:
    return F::R16_FLOAT;
  case TF::k_16_16_FLOAT:
    return F::R16G16_FLOAT;
  case TF::k_16_16_16_16_FLOAT:
    return F::R16G16B16A16_FLOAT;
  case TF::k_32_FLOAT:
    return F::R32_FLOAT;
  case TF::k_32_32_FLOAT:
    return F::R32G32_FLOAT;
  case TF::k_32_32_32_32_FLOAT:
    return F::R32G32B32A32_FLOAT;
  case TF::k_24_8:
  case TF::k_24_8_FLOAT:
    return Video::DepthStencilFormat();
  case TF::k_DXT1:
    return gamma ? F::BC1_UNORM_SRGB : F::BC1_UNORM;
  case TF::k_DXT2_3:
    return gamma ? F::BC2_UNORM_SRGB : F::BC2_UNORM;
  case TF::k_DXT4_5:
    return gamma ? F::BC3_UNORM_SRGB : F::BC3_UNORM;
  default:
    return F::UNKNOWN;
  }
}

bool IsRenderTargetCapable(plume::RenderFormat format) {
  // BCn is not RT-capable (D3D12 rejects RENDER_TARGET on it).
  switch (format) {
  case plume::RenderFormat::R8G8B8A8_UNORM_SRGB:
  case plume::RenderFormat::R8G8_UNORM:
  case plume::RenderFormat::R16G16_UNORM:
  case plume::RenderFormat::R16_FLOAT:
  case plume::RenderFormat::R32_FLOAT:
  case plume::RenderFormat::R32G32_FLOAT:
  case plume::RenderFormat::R8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::B8G8R8A8_UNORM:
  case plume::RenderFormat::R16G16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
  case plume::RenderFormat::R32G32B32A32_FLOAT:
  case plume::RenderFormat::R11G11B10_FLOAT:
    return true;
  default:
    return false;
  }
}

const char *ConvertDeclUsage(D3DDeclUsage usage) {
  switch (usage) {
  case D3DDeclUsage::kPosition:
    return "POSITION";
  case D3DDeclUsage::kBlendWeight:
    return "BLENDWEIGHT";
  case D3DDeclUsage::kBlendIndices:
    return "BLENDINDICES";
  case D3DDeclUsage::kNormal:
    return "NORMAL";
  case D3DDeclUsage::kPSize:
    return "PSIZE";
  case D3DDeclUsage::kTexCoord:
    return "TEXCOORD";
  case D3DDeclUsage::kTangent:
    return "TANGENT";
  case D3DDeclUsage::kBinormal:
    return "BINORMAL";
  case D3DDeclUsage::kTessFactor:
    return "TESSFACTOR";
  case D3DDeclUsage::kPositionT:
    return "POSITIONT";
  case D3DDeclUsage::kColor:
    return "COLOR";
  case D3DDeclUsage::kFog:
    return "FOG";
  case D3DDeclUsage::kDepth:
    return "DEPTH";
  case D3DDeclUsage::kSample:
    return "SAMPLE";
  default:
    return "UNKNOWN";
  }
}

plume::RenderFormat ConvertDeclType(D3DDeclType type) {
  switch (type) {
  case D3DDeclType::kFloat1:
    return plume::RenderFormat::R32_FLOAT;
  case D3DDeclType::kFloat2:
    return plume::RenderFormat::R32G32_FLOAT;
  case D3DDeclType::kFloat3:
    return plume::RenderFormat::R32G32B32_FLOAT;
  case D3DDeclType::kFloat4:
    return plume::RenderFormat::R32G32B32A32_FLOAT;
  case D3DDeclType::kD3DColor:
    return plume::RenderFormat::B8G8R8A8_UNORM;
  case D3DDeclType::kUByte4:
  case D3DDeclType::kUByte4Alt:
    return plume::RenderFormat::R8G8B8A8_UINT;
  case D3DDeclType::kShort2:
    return plume::RenderFormat::R16G16_SINT;
  // SHORT4 (non-normalized) binds as SNORM, not SINT: the recompiled BD
  // shaders declare every SHORT4 input as float4, and an integer class IA
  // format against a float class shader input is a D3D12 contract violation.
  case D3DDeclType::kShort4:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUByte4N:
  case D3DDeclType::kUByte4NAlt:
    return plume::RenderFormat::R8G8B8A8_UNORM;
  case D3DDeclType::kShort2N:
    return plume::RenderFormat::R16G16_SNORM;
  case D3DDeclType::kShort4N:
    return plume::RenderFormat::R16G16B16A16_SNORM;
  case D3DDeclType::kUShort2N:
    return plume::RenderFormat::R16G16_UNORM;
  case D3DDeclType::kUShort4N:
    return plume::RenderFormat::R16G16B16A16_UNORM;
  case D3DDeclType::kUInt1:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kDec3NAlt:
  case D3DDeclType::kDec3NAlt2:
    return plume::RenderFormat::R32_UINT;
  case D3DDeclType::kFloat16_2:
    return plume::RenderFormat::R16G16_FLOAT;
  case D3DDeclType::kFloat16_4:
    return plume::RenderFormat::R16G16B16A16_FLOAT;
  default:
    return plume::RenderFormat::UNKNOWN;
  }
}

} // namespace mgrn::gpu
