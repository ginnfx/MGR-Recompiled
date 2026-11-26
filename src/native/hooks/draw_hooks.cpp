// Native renderer: draws, clears, resolves and present.
//
// Draw, Clear and Resolve replace the guest bodies (which would only build PM4
// for the emulator); Swap presents and then runs the guest body, so the flip
// queue, VdSwap and the game's frame pacing behave exactly as before.
// DispatchDraw adapted from re:Blue gpu/hooks/draw.cpp (BSD-3, Copyright (c)
// 2026 Tom Clay).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/byte_swap.h"
#include "gpu/capture.h"
#include "gpu/d3d.h"
#include "gpu/depth_probe.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/host_resource_heap.h"
#include "gpu/settings.h"
#include "gpu/upscale.h"
#include "gpu/vertex_declaration.h"
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

// Presents so far; the Swap hook bumps it.
std::atomic<u32> g_present_count{0};

// Dev: MGR_NATIVE_DRAW_DUMP=<present> logs every draw of that frame that got
// past framebuffer bind and state flush (dropped draws log as [draw-diag]).
u32 DrawDumpFrame() {
  static const u32 configured_frame = [] {
    const char* value = std::getenv("MGR_NATIVE_DRAW_DUMP");
    return (mgrn::DevMode() && value) ? u32(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  // Optional live capture: write a new positive request ID to this file.
  // Poll once per present, only when explicitly enabled in dev mode. This
  // captures the next complete draw sequence without relaunching the game.
  static const char* trigger = mgrn::DevMode() ? std::getenv("MGR_NATIVE_DRAW_TRIGGER") : nullptr;
  if (!trigger || !*trigger) return configured_frame;
  static u32 polled_present = ~0u;
  static u32 requested_frame = 0;
  static u64 last_request = 0;
  const u32 present = g_present_count.load(std::memory_order_relaxed);
  if (polled_present != present) {
    polled_present = present;
    u64 request = 0;
    std::ifstream file(trigger);
    if (file >> request; request > last_request) {
      last_request = request;
      requested_frame = present;
      MGRN_WARN("[draw-trigger] request {} present {}", request, present);
    }
  }
  return requested_frame ? requested_frame : configured_frame;
}

// Optional external GPU-capture handshake. Format:
// MGR_NATIVE_CAPTURE_GATE="<next-present>|<ready-file>|<release-file>".
// After the selected present has been submitted and the present counter moves
// to next-present, notify the controller. If release-file is nonempty, wait
// before guest execution starts the next frame. This adds no GPU commands or
// render-pass splits.
void WaitForExternalCapture(u32 next_present) {
  static bool handled = false;
  if (handled || !mgrn::DevMode()) return;
  const char* spec = std::getenv("MGR_NATIVE_CAPTURE_GATE");
  if (!spec || !*spec) return;
  const char* first_sep = std::strchr(spec, '|');
  if (!first_sep) return;
  const char* second_sep = std::strchr(first_sep + 1, '|');
  if (!second_sep) return;
  char* end = nullptr;
  const u32 target = u32(std::strtoul(spec, &end, 10));
  if (end != first_sep || target != next_present) return;
  handled = true;
  const std::string ready_path(first_sep + 1, second_sep);
  const std::string release_path(second_sep + 1);
  {
    std::ofstream ready(ready_path, std::ios::trunc);
    ready << next_present << '\n';
  }
  if (release_path.empty()) {
    MGRN_WARN("[gpu-capture-gate] present {} notified", next_present);
    return;
  }
  MGRN_WARN("[gpu-capture-gate] present {} paused for external capture", next_present);
  for (;;) {
    std::ifstream release(release_path);
    if (release.good()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  MGRN_WARN("[gpu-capture-gate] present {} resumed", next_present);
}

u64 ShaderHash(const gpu::GuestShader* shader) {
  return (shader && shader->shaderCacheEntry) ? shader->shaderCacheEntry->hash : 0;
}


void DumpDraw(u32 device_guest, const gpu::VideoState& s, u32 primitive_type,
              const DrawArgs& args, u32 draw_index) {
  const auto& ps = s.pipelineState;
  const auto* rt = s.render_target;
  const auto* ds = s.depth_stencil;
  const auto& vp = s.viewport;
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  const u32 mode_control = dev ? gpu::ReadDeviceRegister(*dev, 0x2205) : 0;
  const u32 reset_index = dev ? gpu::ReadDeviceRegister(*dev, 0x2103) : 0;
  const auto* ib = s.index_buffer;
  u32 block_stream_mask = 0;
  const auto* decl = s.pipelineState.vertexDeclaration;
  if (decl && decl->vertexElements) {
    for (u32 i = 0; i < decl->vertexElementCount; ++i) {
      const u32 stream = u32(decl->vertexElements[i].stream);
      if (stream < mgrn::native::kBoundStreamCount) {
        const auto* vb = mgrn::native::BoundStreamAt(stream).buffer;
        if (vb && vb->blockBuffer) block_stream_mask |= 1u << stream;
      }
    }
  }
  MGRN_WARN("[draw-dump] #{} prim={} {}={} vs={:016X} ps={:016X} decl={:016X}{} "
            "rt={}x{}/{}s{} ds={}/{}s{}@{} z={}{}f{} bias={}/{:.3f} cull={} blend={} cw=0x{:X} "
            "vp={:.0f},{:.0f} {:.0f}x{:.0f} z{:.2f}-{:.2f} "
            "primReset={} reset=0x{:06X} ibElem={} ibBlock={} vbBlocks=0x{:04X}",
            draw_index, primitive_type,
            args.indexed ? "idx" : "vtx", args.vertex_or_index_count,
            ShaderHash(ps.vertexShader), ShaderHash(ps.pixelShader),
            ps.vertexDeclaration ? ps.vertexDeclaration->hash : 0,
            gpu::IsSkinnedDeclaration(ps.vertexDeclaration) ? " SKINNED" : "", rt ? rt->width : 0,
            rt ? rt->height : 0, rt ? u32(rt->format) : 0, rt ? u32(rt->sampleCount) : 0,
            ds ? ds->width : 0, ds ? u32(ds->format) : 0, ds ? u32(ds->sampleCount) : 0,
            static_cast<const void*>(ds), ps.zEnable ? 1 : 0, ps.zWriteEnable ? "w" : "",
            u32(ps.zFunc), ps.depthBias, ps.slopeScaledDepthBias, u32(ps.cullMode), ps.alphaBlendEnable ? 1 : 0,
            ps.colorWriteEnable, vp.x, vp.y, vp.width, vp.height, vp.minDepth,
            vp.maxDepth, (mode_control >> 21) & 1, reset_index & 0xFFFFFF,
            ib ? ib->elementSize : 0, ib && ib->blockBuffer ? 1 : 0,
            block_stream_mask);
}

// Bound textures of the draw as slot:va fmt(guest) WxH (host size), for
// finding which pass feeds a full-screen shader.
std::string DescribeBoundTextures(const gpu::VideoState& s) {
  std::string out;
  for (u32 i = 0; i < gpu::kFetchSlotCount; ++i) {
    const gpu::GuestTexture* t = s.textures[i];
    if (!t) continue;
    out += std::format(" s{}=0x{:08X}/g{}/{}x{}{}", i, t->selfVa, t->guestFormat, t->width,
                       t->height, t->sourceSurface ? "/rt" : "");
  }
  return out;
}

// Each element as stream:offset usage[index] type.
std::string DescribeDecl(const gpu::GuestVertexDeclaration* decl) {
  std::string out;
  if (!decl || !decl->vertexElements)
    return out;
  for (u32 i = 0; i < decl->vertexElementCount; ++i) {
    const auto& e = decl->vertexElements[i];
    out += std::format(" {}:{} u{}[{}] 0x{:06X}", u32(e.stream), u32(e.offset), u32(e.usage),
                       u32(e.usageIndex), u32(e.type));
  }
  return out;
}

// Streams compared in the skinned-draw dump; MGR's skinned decls use 0..1.
constexpr u32 kDumpStreamCount = 4;

// First bone-palette register (g_WorldArray) in MGR's skinned vertex shaders.
constexpr u32 kBonePaletteFirstReg = 40;
constexpr u32 kBonePaletteRegs = 216;

// For skinned draws: the first bone matrix from the device constant shadow and
// how many palette registers are non-zero, to see whether bones reach it.
void DumpBonePalette(u32 device_guest) {
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  if (!dev)
    return;
  u32 nonzero = 0;
  for (u32 r = kBonePaletteFirstReg; r < kBonePaletteFirstReg + kBonePaletteRegs; ++r)
    for (u32 c = 0; c < 4; ++c)
      if (float(dev->vsFloatConstants[r][c]) != 0.0f) {
        ++nonzero;
        break;
      }
  const auto& c = dev->vsFloatConstants;
  const u32 b = kBonePaletteFirstReg;
  MGRN_WARN("[draw-dump]   bones nonzero_regs={}/{} c40=({:.3f},{:.3f},{:.3f},{:.3f}) "
            "c41=({:.3f},{:.3f},{:.3f},{:.3f}) c42=({:.3f},{:.3f},{:.3f},{:.3f})",
            nonzero, kBonePaletteRegs, float(c[b][0]), float(c[b][1]), float(c[b][2]),
            float(c[b][3]), float(c[b + 1][0]), float(c[b + 1][1]), float(c[b + 1][2]),
            float(c[b + 1][3]), float(c[b + 2][0]), float(c[b + 2][1]), float(c[b + 2][2]),
            float(c[b + 2][3]));
}

// Dev: four consecutive VS float registers (a matrix) from the device shadow.
void DumpVsMatrix(u32 device_guest, const char* name, u32 first) {
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  if (!dev)
    return;
  const auto& c = dev->vsFloatConstants;
  std::string rows;
  for (u32 r = first; r < first + 4; ++r)
    rows += fmt::format(" c{}=({:.4f},{:.4f},{:.4f},{:.4f})", r, float(c[r][0]), float(c[r][1]),
                        float(c[r][2]), float(c[r][3]));
  MGRN_WARN("[draw-dump]   {}{}", name, rows);
}

// Dev: MGR_NATIVE_SKIP_SKINNED_PREPASS=1 drops skinned draws with colour
// writes off (depth pre-pass and shadow casters), to test their depth; =2
// drops every colour-off draw.
char SkipPrepassMode() {
  static const char mode = [] {
    const char* value = std::getenv("MGR_NATIVE_SKIP_SKINNED_PREPASS");
    return (mgrn::DevMode() && value) ? value[0] : '0';
  }();
  return mode;
}

// Dev bisection: MGR_NATIVE_SKIP_DRAWS=skinned-colour | static-colour |
// <vs hash>[,<vs hash>...] drops those colour draws.
// draw:<first>-<last> drops an inclusive range of successfully flushed draws
// each present (zero based, before filtering), including depth-only draws.
// Comma-separated ranges/single ordinals are supported, e.g. draw:1-2,432,452.
bool SkipBisectedDraw(const gpu::PipelineState& ps, u32 draw) {
  static const std::string spec = [] {
    const char* value = std::getenv("MGR_NATIVE_SKIP_DRAWS");
    return (mgrn::DevMode() && value) ? std::string(value) : std::string();
  }();
  if (spec.empty())
    return false;
  if (spec.starts_with("draw:")) {
    const char* at = spec.c_str() + 5;
    bool selected = false;
    for (;;) {
      char* end = nullptr;
      const auto first = std::strtoul(at, &end, 10);
      if (end == at) return false;
      auto last = first;
      if (*end == '-') {
        at = end + 1;
        last = std::strtoul(at, &end, 10);
        if (end == at) return false;
      }
      selected |= draw >= first && draw <= last;
      if (*end == '\0') return selected;
      if (*end != ',') return false;
      at = end + 1;
    }
  }
  // vb:<hex base VA> drops every draw (depth-only included) reading that buffer.
  static const u32 skip_buffer_va = [] {
    const size_t at = spec.find("vb:");
    return at == std::string::npos ? 0u : u32(std::strtoul(spec.c_str() + at + 3, nullptr, 16));
  }();
  if (skip_buffer_va && mgrn::native::DrawReadsGuestBuffer(skip_buffer_va))
    return true;
  if (ps.colorWriteEnable == 0)
    return false;
  const bool skinned = gpu::IsSkinnedDeclaration(ps.vertexDeclaration);
  if (spec == "skinned-colour")
    return skinned;
  if (spec == "static-colour")
    return !skinned;
  const std::string hash = std::format("{:016X}", ShaderHash(ps.vertexShader));
  return spec.find(hash) != std::string::npos;
}

bool SkipPrepassDraw(const gpu::PipelineState& ps) {
  const char mode = SkipPrepassMode();
  if (ps.colorWriteEnable != 0 || (mode != '1' && mode != '2'))
    return false;
  return mode == '2' || gpu::IsSkinnedDeclaration(ps.vertexDeclaration);
}

// Dev: in the dump frame, read back the depth target before the first
// depth-tested colour draw of each kind (skinned, then static).
void ProbeDepthBeforeDraw(gpu::VideoState& s) {
  static bool probed_skinned = false;
  static bool probed_static = false;
  const auto& ps = s.pipelineState;
  const bool skinned = gpu::IsSkinnedDeclaration(ps.vertexDeclaration);
  if (ps.zEnable && ps.zWriteEnable && ps.colorWriteEnable == 0) {
    // Depth-only draws: probe after the first of each kind, to see which one
    // covers the target.
    static u32 depth_only_seen = 0;
    ++depth_only_seen;
    if (depth_only_seen <= 2 || (depth_only_seen >= 24 && depth_only_seen <= 31)) {
      const std::string tag = fmt::format("before-depth-only-{}-{}", depth_only_seen,
                                          skinned ? "skinned" : "static");
      gpu::RecordDepthProbeLocked(s, s.depth_stencil, tag.c_str());
    }
    return;
  }
  if (!ps.zEnable || ps.colorWriteEnable == 0)
    return;
  bool& probed = skinned ? probed_skinned : probed_static;
  if (probed)
    return;
  probed = true;
  gpu::RecordDepthProbeLocked(s, s.depth_stencil,
                              skinned ? "before-skinned-colour" : "before-static-colour");
}

// Dev: logs (rate-limited) VS constant registers holding +-inf at draw time.
// Xenos multiplies 0 * inf to 0; the recompiled shaders give NaN.
void CheckInfConstants(u32 device_guest, u32 present, u64 vs_hash) {
  constexpr u32 kMaxReports = 100;
  static std::atomic<u32> reported{0};
  if (reported.load(std::memory_order_relaxed) >= kMaxReports)
    return;
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  if (!dev)
    return;
  for (u32 r = 0; r < 256; ++r)
    for (u32 c = 0; c < 4; ++c)
      if (std::isinf(float(dev->vsFloatConstants[r][c]))) {
        if (reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
          MGRN_WARN("[inf-const] present {} vs={:016X} c{}.{} = {}", present, vs_hash, r,
                    "xyzw"[c], float(dev->vsFloatConstants[r][c]));
        return;
      }
}

// Dev: logs each (vs, ps) pair drawn in a frame that the previous frame did
// not draw, to catch draws that only occur in some frames.
void NoteRareShaderPair(const gpu::VideoState& s, u32 present, u32 primitive_type,
                        const DrawArgs& args) {
  static std::unordered_set<u64> previous, current;
  static u32 current_present = 0;
  static std::atomic<u32> reported{0};
  constexpr u32 kMaxReports = 2000;
  // Present 4700 onward: idle gameplay on the census route.
  constexpr u32 kFirstPresent = 4700;
  if (present != current_present) {
    previous.swap(current);
    current.clear();
    current_present = present;
  }
  const auto& ps = s.pipelineState;
  const u64 vs = ShaderHash(ps.vertexShader), pix = ShaderHash(ps.pixelShader);
  const u64 key = vs * 0x9E3779B97F4A7C15ull ^ pix;
  if (!current.insert(key).second || present < kFirstPresent || previous.count(key) ||
      reported.fetch_add(1, std::memory_order_relaxed) >= kMaxReports)
    return;
  MGRN_WARN("[rare-pair] present {} vs={:016X} ps={:016X} prim={} {}={} blend={} cw=0x{:X} "
            "decl={:016X}{}",
            present, vs, pix, primitive_type, args.indexed ? "idx" : "vtx",
            args.vertex_or_index_count, ps.alphaBlendEnable ? 1 : 0, ps.colorWriteEnable,
            ps.vertexDeclaration ? ps.vertexDeclaration->hash : 0,
            gpu::IsSkinnedDeclaration(ps.vertexDeclaration) ? " SKINNED" : "");
}

// The guest IB struct keeps flags in the low two bits of its size, so a
// recorded size can be up to 3 bytes short: an odd 16-bit index count loses
// its last index. Xenos fetches by draw count, never by buffer size.
constexpr u32 kIndexSizeMaskedBytes = 3;

// Indices a draw may fetch from first_byte on, counting the masked tail.
u32 GuestIndicesAvailable(const gpu::GuestBuffer& ib, u64 first_byte, u32 elem) {
  const u64 end = u64(ib.dataSize) + kIndexSizeMaskedBytes;
  return first_byte < end ? u32((end - first_byte) / elem) : 0;
}

// Indexed draw reaching past the host copy of its IB (the masked tail): the
// GPU would read past that buffer and turn the last strip triangle into a
// stray streak. Fetch the draw's indices from guest memory into the upload
// heap instead, as Xenos does. False when they cannot be read. Caller holds
// s.mutex.
bool DrawIndexedFromGuest(gpu::VideoState& s, plume::RenderCommandList* cmd_list,
                          const DrawArgs& args) {
  const gpu::GuestBuffer* ib = s.index_buffer;
  if (!ib || !ib->guestMirrorVa) return false;
  const u32 elem = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
  const u64 first_byte = u64(args.start_index) * elem;
  const u32 count =
      std::min(args.vertex_or_index_count, GuestIndicesAvailable(*ib, first_byte, elem));
  const auto* src = mgrn::mem::try_at<const u8>(ib->guestMirrorVa + u32(first_byte));
  if (!count || !src) return false;
  std::vector<u8> swapped(size_t(count) * elem);
  gpu::ByteSwapIndexElements(swapped.data(), src, u32(swapped.size()), elem);
  const auto alloc = gpu::UploadHostBytes(swapped.data(), u32(swapped.size()));
  if (!alloc.size) return false;
  plume::RenderIndexBufferView view{};
  view.buffer = alloc.ref;
  view.size = u32(swapped.size());
  view.format = s.index_view.format;
  cmd_list->setIndexBuffer(&view);
  cmd_list->drawIndexedInstanced(count, 1, 0, args.base_vertex_index, 0);
  // The uploaded IB replaced the tracked guest IB on the command list.
  s.dirtyStates.indices = true;
  return true;
}

// Indexed quad list: expands the guest's indices (4 per quad) into two
// triangles per quad in the upload heap, since the shared expansion IB only
// covers sequential vertices. Caller holds s.mutex.
void DrawIndexedQuadList(gpu::VideoState& s, plume::RenderCommandList* cmd_list,
                         const DrawArgs& args) {
  const gpu::GuestBuffer* ib = s.index_buffer;
  if (!ib || !ib->guestMirrorVa) return;
  const u32 elem = ib->format == plume::RenderFormat::R32_UINT ? 4u : 2u;
  const u64 first_byte = u64(args.start_index) * elem;
  const u32 available = GuestIndicesAvailable(*ib, first_byte, elem);
  const u32 quads = std::min(std::min(args.vertex_or_index_count, available) / 4,
                             gpu::Video::QuadlistMaxQuads());
  const auto* src = mgrn::mem::try_at<const u8>(ib->guestMirrorVa + u32(first_byte));
  if (!quads || !src) return;
  auto index_at = [&](u32 n) {
    const u8* p = src + u64(n) * elem;
    return elem == 4 ? u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]
                     : u32(p[0]) << 8 | p[1];
  };
  constexpr u32 kCorners[6] = {0, 1, 2, 0, 2, 3};
  std::vector<u32> expanded(quads * 6);
  for (u32 q = 0; q < quads; ++q)
    for (u32 k = 0; k < 6; ++k) expanded[q * 6 + k] = index_at(q * 4 + kCorners[k]);
  const u32 bytes = u32(expanded.size() * sizeof(u32));
  const auto alloc = gpu::UploadHostBytes(expanded.data(), bytes);
  if (!alloc.size) return;
  plume::RenderIndexBufferView view{};
  view.buffer = alloc.ref;
  view.size = bytes;
  view.format = plume::RenderFormat::R32_UINT;
  cmd_list->setIndexBuffer(&view);
  cmd_list->drawIndexedInstanced(quads * 6, 1, 0, args.base_vertex_index, 0);
  // The expanded IB replaced the tracked guest IB on the command list.
  s.dirtyStates.indices = true;
}

void DispatchDraw(u32 device_guest, u32 primitive_type, const DrawArgs& args) {
  mgrn::native::RefreshBoundGeometry(device_guest, args.indexed);
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

  // The composite's first draw upscales the scene first, which can submit the
  // frame so far and redirect the bound target.
  gpu::upscale::BeforeDrawLocked(s, device_guest);
  if (!s.command_list_open) return;
  if (!gpu::Video::BindDrawFramebufferLocked()) return;
  if (!gpu::Video::FlushRenderStateLocked(device_guest)) return;

  auto* cmd_list = s.command_list;
  if (!cmd_list) return;
  // Stable ordinals include suppressed draws, so logs, captures and bisection
  // refer to the same draws when a filter is enabled.
  static u32 ordinal_present = ~0u;
  static u32 next_ordinal = 0;
  const u32 present = g_present_count.load(std::memory_order_relaxed);
  if (present != ordinal_present) {
    ordinal_present = present;
    next_ordinal = 0;
  }
  const u32 draw_ordinal = next_ordinal++;
  const bool skip_bisected = SkipBisectedDraw(s.pipelineState, draw_ordinal);
  if (SkipPrepassDraw(s.pipelineState) || skip_bisected)
    return;
  if (mgrn::DevMode())
    NoteRareShaderPair(s, g_present_count.load(std::memory_order_relaxed), primitive_type, args);
  if (mgrn::DevMode())
    mgrn::native::CheckIndexStability(args.indexed, args.start_index, args.vertex_or_index_count,
                                      g_present_count.load(std::memory_order_relaxed));
  if (mgrn::DevMode())
    CheckInfConstants(device_guest, g_present_count.load(std::memory_order_relaxed),
                      ShaderHash(s.pipelineState.vertexShader));
  if (mgrn::DevMode() && gpu::IsSkinnedDeclaration(s.pipelineState.vertexDeclaration))
    mgrn::native::CheckSkinSanity(device_guest, s.pipelineState.vertexDeclaration, args.indexed,
                                  args.indexed ? args.start_index : args.start_vertex,
                                  args.vertex_or_index_count, args.base_vertex_index,
                                  kBonePaletteFirstReg, kBonePaletteRegs,
                                  g_present_count.load(std::memory_order_relaxed),
                                  ShaderHash(s.pipelineState.vertexShader));
  if (mgrn::DevMode())
    mgrn::native::CheckStreamBounds(s.pipelineState.vertexDeclaration, args.indexed,
                                    args.indexed ? args.start_index : args.start_vertex,
                                    args.vertex_or_index_count, args.base_vertex_index,
                                    g_present_count.load(std::memory_order_relaxed));
  const u32 dump_frame = DrawDumpFrame();
  if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame) {
    ProbeDepthBeforeDraw(s);
    mgrn::native::NoteDrawGeometry(draw_ordinal);
    DumpDraw(device_guest, s, primitive_type, args, draw_ordinal);
    MGRN_WARN("[draw-dump]   tex{}", DescribeBoundTextures(s));
    if (gpu::IsSkinnedDeclaration(s.pipelineState.vertexDeclaration) ||
        s.pipelineState.colorWriteEnable == 0) {
      DumpBonePalette(device_guest);
      DumpVsMatrix(device_guest, "proj", 4);
      DumpVsMatrix(device_guest, "worldview", 20);
      MGRN_WARN("[draw-dump]   streams{}",
                mgrn::native::DescribeBoundStreams(device_guest, kDumpStreamCount));
      MGRN_WARN("[draw-dump]   decl{}", DescribeDecl(s.pipelineState.vertexDeclaration));
      if (gpu::IsSkinnedDeclaration(s.pipelineState.vertexDeclaration))
        MGRN_WARN("[draw-dump]   skin{}",
                  mgrn::native::CheckSkinReferences(
                      device_guest, s.pipelineState.vertexDeclaration, args.indexed,
                      args.indexed ? args.start_index : args.start_vertex,
                      args.vertex_or_index_count, args.base_vertex_index,
                      kBonePaletteFirstReg, kBonePaletteRegs));
    }
  }

  // Selected draw range in the dump frame, copied without a per-draw CPU wait.
  auto capture_draw_target = [&] {
    const char* range = std::getenv("MGR_NATIVE_DRAW_CAPTURE");
    const char* dir = std::getenv("MGR_CAPTURE_DIR");
    if (!mgrn::DevMode() || !dump_frame || present != dump_frame || !range || !dir) return;
    char* end = nullptr;
    const u32 first = u32(std::strtoul(range, &end, 10));
    if (end == range) return;
    const u32 last = *end == '-' ? u32(std::strtoul(end + 1, &end, 10)) : first;
    const u32 step = *end == ':' ? std::max(1u, u32(std::strtoul(end + 1, nullptr, 10))) : 1u;
    if (draw_ordinal >= first && draw_ordinal <= last && (draw_ordinal - first) % step == 0) {
      const auto prefix = std::format("{}/draw-{}-{}", dir, dump_frame, draw_ordinal);
      gpu::RecordDrawCapture(s, s.render_target, prefix + ".ppm");
      const char* geometry = std::getenv("MGR_NATIVE_DRAW_GEOMETRY");
      if (geometry && std::string_view(geometry) == "1")
        mgrn::native::DumpDrawInputs(device_guest, prefix, args.indexed,
            args.indexed ? args.start_index : args.start_vertex,
            args.vertex_or_index_count, args.base_vertex_index);
      const char* dump_decl = std::getenv("MGR_NATIVE_DRAW_DECL");
      if (dump_decl && std::string_view(dump_decl) == "1") {
        std::ofstream decl_file(prefix + "-decl.txt");
        decl_file << "hash=" << std::format("{:016X}",
            s.pipelineState.vertexDeclaration ? s.pipelineState.vertexDeclaration->hash : 0)
                  << " elements=" << DescribeDecl(s.pipelineState.vertexDeclaration) << '\n';
      }
    }
  };

  if (static_cast<xe::PrimitiveType>(primitive_type) == xe::PrimitiveType::kQuadList &&
      args.indexed) {
    DrawIndexedQuadList(s, cmd_list, args);
  } else if (static_cast<xe::PrimitiveType>(primitive_type) == xe::PrimitiveType::kQuadList) {
    const u32 quads = std::min(args.vertex_or_index_count / 4, gpu::Video::QuadlistMaxQuads());
    const auto* ib = gpu::Video::QuadlistExpansionIBView();
    if (quads && ib) {
      cmd_list->setIndexBuffer(ib);
      cmd_list->drawIndexedInstanced(quads * 6, 1, 0, i32(args.start_vertex), 0);
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
    const u32 elem = s.index_view.format == plume::RenderFormat::R32_UINT ? 4u : 2u;
    const u64 end_byte = (u64(args.start_index) + args.vertex_or_index_count) * elem;
    if (end_byte <= s.index_view.size || !DrawIndexedFromGuest(s, cmd_list, args))
      cmd_list->drawIndexedInstanced(args.vertex_or_index_count, 1, args.start_index,
                                     args.base_vertex_index, 0);
  } else {
    cmd_list->drawInstanced(args.vertex_or_index_count, 1, args.start_vertex, 0);
  }
  capture_draw_target();
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
// Count big-endian D3DRECTs (x1, y1, x2, y2) at rects_va, in guest pixels.
std::vector<plume::RenderRect> ReadClearRects(u32 count, u32 rects_va) {
  constexpr u32 kMaxClearRects = 64;
  std::vector<plume::RenderRect> rects;
  if (!count || !rects_va) return rects;
  const auto* raw = mgrn::mem::try_at<const be_i32>(rects_va);
  if (!raw) return rects;
  count = std::min(count, kMaxClearRects);
  for (u32 i = 0; i < count; ++i)
    rects.emplace_back(i32(raw[i * 4 + 0]), i32(raw[i * 4 + 1]), i32(raw[i * 4 + 2]),
                       i32(raw[i * 4 + 3]));
  return rects;
}

// Resolve-and-clear: what D3DDevice_Resolve clears after its copy.
constexpr u32 kResolveClearRenderTarget = 0x100u;
constexpr u32 kResolveClearDepthStencil = 0x200u;
// ClearStencil is the tenth argument: the caller's stack, where the guest
// body loads it (lwz r29,252(r1) after its 160-byte stwu).
constexpr u32 kResolveClearStencilStackOffset = 92;

void ResolveClear(const PPCContext& ctx) {
  const u32 flags = ctx.r4.u32;
  u32 clear = 0;
  plume::RenderColor color{};
  if ((flags & kResolveClearRenderTarget) && ctx.r10.u32) {
    if (const auto* c = mgrn::mem::try_at<const be_f32>(ctx.r10.u32)) {
      color = plume::RenderColor(float(c[0]), float(c[1]), float(c[2]), float(c[3]));
      clear |= gpu::kClearTarget;
    }
  }
  if (flags & kResolveClearDepthStencil)
    clear |= gpu::kClearZBuffer | gpu::kClearStencil;
  if (!clear) return;
  const auto* stencil = mgrn::mem::try_at<const be_u32>(ctx.r1.u32 + kResolveClearStencilStackOffset);
  gpu::Video::Clear(clear, color, float(ctx.f1.f64), stencil ? u32(*stencil) : 0u);
}

MGRN_EXTERN(D3DDevice_Clear);
MGRN_HOOK_RAW(D3DDevice_Clear) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_Clear)(ctx, base);
  const u32 dump_frame = DrawDumpFrame();
  if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame) {
    const auto& s = gpu::state();
    MGRN_WARN("[draw-dump] CLEAR flags=0x{:X} color=0x{:08X} z={:.3f} stencil={} rt={} ds={}",
              ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r9.u32,
              static_cast<const void*>(s.render_target), static_cast<const void*>(s.depth_stencil));
  }
  gpu::Video::Clear(ctx.r6.u32, ctx.r7.u32, float(ctx.f1.f64), ctx.r9.u32,
                    ReadClearRects(ctx.r4.u32, ctx.r5.u32));
  if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame &&
      (ctx.r6.u32 & gpu::kClearZBuffer)) {
    auto& s = gpu::state();
    std::lock_guard lock(s.mutex);
    gpu::RecordDepthProbeLocked(s, s.depth_stencil, "after-depth-clear");
  }
}

// D3DDevice_Resolve(pDevice, Flags, pSourceRect, pDestTexture, pDestPoint,
// DestLevel, DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters).
MGRN_EXTERN(D3DDevice_Resolve);
MGRN_HOOK_RAW(D3DDevice_Resolve) {
  if (!NativeRendererActive()) return MGRN_IMP(D3DDevice_Resolve)(ctx, base);
  const u32 flags = ctx.r4.u32, dest = ctx.r6.u32, level = ctx.r8.u32, slice = ctx.r9.u32;
  gpu::GuestTexture* dst = mgrn::native::ResolveDestination(dest);
  const u32 dump_frame = DrawDumpFrame();
  if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame) {
    const auto* r = ctx.r5.u32 ? mgrn::mem::try_at<const be_u32>(ctx.r5.u32) : nullptr;
    const auto* p = ctx.r7.u32 ? mgrn::mem::try_at<const be_u32>(ctx.r7.u32) : nullptr;
    const auto* src = gpu::state().render_target;
    MGRN_WARN("[draw-dump] RESOLVE flags=0x{:X} dest=0x{:08X} host={} {}x{}/{} src={}x{} rect={} point={}",
              flags, dest, static_cast<const void*>(dst), dst ? dst->width : 0,
              dst ? dst->height : 0, dst ? u32(dst->format) : 0, src ? src->width : 0,
              src ? src->height : 0,
              r ? fmt::format("{},{},{},{}", u32(r[0]), u32(r[1]), u32(r[2]), u32(r[3])) : "full",
              p ? fmt::format("{},{}", u32(p[0]), u32(p[1])) : "0,0");
  }
  if (!dst) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 16)
      MGRN_WARN("native resolve: destination 0x{:08X} has no host texture", dest);
    ResolveClear(ctx);
    return;
  }
  gpu::Video::TrackResolveSource(flags, dst, level, slice);
  // Xenos copies the source rect 1:1 to the dest point, for colour as for
  // depth: the glare chain resolves an 80x48 corner of its 320x176 target.
  const bool depth_dst = gpu::IsDepthFormat(dst->format);
  auto& state = gpu::state();
  auto* src = (flags & gpu::kResolveDepthStencil) ? state.depth_stencil : state.render_target;
  if (src) {
    plume::RenderRect rect(0, 0, i32(src->width / src->renderScale),
                                i32(src->height / src->renderScale));
    if (ctx.r5.u32) {
      const auto* r = mgrn::mem::try_at<const be_u32>(ctx.r5.u32);
      if (!r) return;
      rect = plume::RenderRect(i32(u32(r[0])), i32(u32(r[1])), i32(u32(r[2])), i32(u32(r[3])));
    }
    i32 x = 0, y = 0;
    if (ctx.r7.u32) {
      const auto* p = mgrn::mem::try_at<const be_u32>(ctx.r7.u32);
      if (!p) return;
      x = i32(u32(p[0])); y = i32(u32(p[1]));
    }
    static std::atomic<u32> noted{0};
    if (depth_dst && noted.fetch_add(1) < 8)
      MGRN_DEV_WARN("[depth-region] dest=0x{:08X} src={},{},{},{} dst={},{} {}x{}",
                dest, rect.left, rect.top, rect.right, rect.bottom, x, y, dst->width, dst->height);
    gpu::Video::ResolveRtToTexture(dst, &rect, x, y);
  } else if (!depth_dst) {
    gpu::Video::ResolveRtToTexture(dst);
  }
  ResolveClear(ctx);
  if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame) {
    auto& s = gpu::state();
    std::lock_guard lock(s.mutex);
    const std::string tag = fmt::format("after-resolve-0x{:X}-to-{}x{}", flags, dst->width,
                                        dst->height);
    gpu::RecordDepthProbeLocked(s, s.depth_stencil, tag.c_str());
  }
}

// D3DDevice_Swap(pDevice, pFrontBuffer, pParameters): pFrontBuffer is the
// texture the engine just resolved the finished frame into.
MGRN_EXTERN(D3DDevice_Swap);
MGRN_HOOK_RAW(D3DDevice_Swap) {
  if (NativeRendererActive()) {
    const u32 dump_frame = DrawDumpFrame();
    if (dump_frame && g_present_count.load(std::memory_order_relaxed) == dump_frame)
      mgrn::native::ReportLateGeometryWrites();
    gpu::Video::Present(mgrn::native::LookupPresentTexture(ctx.r4.u32));
    const u32 next_present = g_present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    WaitForExternalCapture(next_present);
    // Resolution, shadow and MSAA changes take effect from the next frame's targets.
    gpu::RefreshRenderTargetSettings();
    gpu::Video::ArmNextPresent();
  }
  MGRN_IMP(D3DDevice_Swap)(ctx, base);
}
