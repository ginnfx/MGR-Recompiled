// Native renderer: dev-mode geometry diagnostics (MGR_NATIVE_DEV=1).
//
// Checks the vertex and index data a draw is about to read: bound buffer
// ranges, skinning inputs, host copies against guest bytes, and guest writes
// that land after a draw. Each check logs at a bounded rate; none changes
// what is drawn.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

#include <rex/types.h>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/byte_swap.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/physical_buffers.h"
#include "gpu/resources.h"
#include "hooks/resources.h"

namespace gpu = mgrn::gpu;
using mgrn::native::BoundIndexBuffer;
using mgrn::native::BoundStreamAt;
using mgrn::native::BoundStreamInfo;
using mgrn::native::kBoundStreamCount;

bool mgrn::native::DrawReadsGuestBuffer(u32 guest_va) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  for (u32 i = 0; i < kBoundStreamCount; ++i)
    if (const auto* buf = BoundStreamAt(i).buffer; buf && buf->guestMirrorVa == guest_va)
      return true;
  return indices && indices->guestMirrorVa == guest_va;
}

std::string mgrn::native::DescribeBoundStreams(u32 device_guest, u32 stream_count) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
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
  for (u32 i = 0; i < stream_count && i < kBoundStreamCount; ++i) {
    const BoundStreamInfo bound = BoundStreamAt(i);
    out += describe(std::format("s{}", i).c_str(), dev ? u32(dev->streamSources[i]) : 0,
                    bound.buffer);
    if (bound.buffer)
      out += std::format(" base=0x{:08X}+{} stride={} size={}{}", bound.buffer->guestMirrorVa,
                         bound.offset, bound.stride, bound.buffer->dataSize,
                         head_bytes(bound.buffer, bound.offset));
  }
  out += describe("ib", dev ? u32(dev->indexBuffer) : 0, indices);
  if (indices)
    out += std::format(" base=0x{:08X} size={} elem={}{}", indices->guestMirrorVa,
                       indices->dataSize, indices->elementSize, head_bytes(indices, 0));
  return out;
}

namespace {
struct NotedRange {
  u32 draw = 0;
  const char* what = "";
  u32 va = 0;
  u32 size = 0;
  u64 hash = 0;
};
std::vector<NotedRange> g_noted_ranges;

u64 HashGuestRange(u32 va, u32 size) {
  const auto* src = mgrn::mem::try_at<const u8>(va);
  return src ? XXH3_64bits(src, size) : 0;
}
}  // namespace

void mgrn::native::NoteDrawGeometry(u32 draw_index) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  auto note = [&](const char* what, const gpu::GuestBuffer* buf) {
    if (buf && buf->guestMirrorVa && buf->dataSize)
      g_noted_ranges.push_back({draw_index, what, buf->guestMirrorVa, buf->dataSize,
                                HashGuestRange(buf->guestMirrorVa, buf->dataSize)});
  };
  note("s0", BoundStreamAt(0).buffer);
  note("s1", BoundStreamAt(1).buffer);
  note("ib", indices);

  // The host copy must equal the byte-swapped guest bytes at draw time.
  auto compare = [&](const char* what, const gpu::GuestBuffer* buf) {
    if (!buf || !buf->buffer || buf->blockBuffer || !buf->guestMirrorVa || !buf->dataSize)
      return;
    const auto* src = mgrn::mem::try_at<const u8>(buf->guestMirrorVa);
    auto* host = static_cast<const u8*>(buf->buffer->map());
    if (!src || !host)
      return;
    std::vector<u8> expected(buf->dataSize);
    if (buf->type == gpu::ResourceType::IndexBuffer)
      gpu::ByteSwapIndexElements(expected.data(), src, buf->dataSize, buf->elementSize);
    else
      gpu::ByteSwapElements(expected.data(), src, buf->dataSize, buf->elementSize);
    u32 diff = 0, first = ~0u;
    for (u32 i = 0; i < buf->dataSize; ++i)
      if (host[i] != expected[i] && diff++ == 0)
        first = i;
    buf->buffer->unmap();
    if (diff)
      MGRN_WARN("[host-diff] draw #{} {} va=0x{:08X} size={} elem={} differs in {} bytes, "
                "first at +{}",
                draw_index, what, buf->guestMirrorVa, buf->dataSize, buf->elementSize, diff,
                first);
  };
  compare("s0", BoundStreamAt(0).buffer);
  compare("s1", BoundStreamAt(1).buffer);
  compare("ib", indices);
}

void mgrn::native::ReportLateGeometryWrites() {
  u32 changed = 0;
  for (const NotedRange& r : g_noted_ranges) {
    if (HashGuestRange(r.va, r.size) == r.hash)
      continue;
    if (++changed <= 64)
      MGRN_WARN("[late-write] draw #{} {} va=0x{:08X} size={} rewritten after the draw",
                r.draw, r.what, r.va, r.size);
  }
  MGRN_WARN("[late-write] {} of {} noted ranges changed before Swap", changed,
            g_noted_ranges.size());
  g_noted_ranges.clear();
}

namespace {
// Bytes per element of a Xenos vertex format (D3DDECLTYPE bits 0..5); 0 for
// formats MGR's declarations do not use.
u32 VertexFormatBytes(u32 decl_type) {
  switch (decl_type & 0x3Fu) {
    case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 4;
    case 26: case 32: case 34: case 37: return 8;
    case 57: return 12;
    case 35: case 38: return 16;
    default: return 0;
  }
}

// Highest index the draw reads (restart markers skipped), or -1.
i64 MaxDrawIndex(bool indexed, u32 start, u32 count) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  if (!indexed)
    return count ? i64(start) + count - 1 : -1;
  if (!indices || !indices->guestMirrorVa)
    return -1;
  const auto* idata = mgrn::mem::try_at<const u8>(indices->guestMirrorVa);
  if (!idata)
    return -1;
  const u32 elem = indices->elementSize == 4 ? 4 : 2;
  const u32 restart = elem == 4 ? 0xFFFFFFFFu : 0xFFFFu;
  i64 max_index = -1;
  for (u32 n = 0; n < count; ++n) {
    const u64 at = u64(start + n) * elem;
    if (at + elem > indices->dataSize)
      break;
    const u32 v = elem == 4 ? u32(idata[at]) << 24 | u32(idata[at + 1]) << 16 |
                                  u32(idata[at + 2]) << 8 | idata[at + 3]
                            : u32(idata[at]) << 8 | idata[at + 1];
    if (v != restart)
      max_index = std::max<i64>(max_index, v);
  }
  return max_index;
}
}  // namespace

void mgrn::native::CheckStreamBounds(const gpu::GuestVertexDeclaration* decl, bool indexed,
                                     u32 start, u32 count, i32 base_vertex, u32 present) {
  static std::atomic<u32> reported{0};
  constexpr u32 kMaxReports = 200;
  if (!decl || !decl->vertexElements || reported.load(std::memory_order_relaxed) >= kMaxReports)
    return;
  const i64 max_index = MaxDrawIndex(indexed, start, count);
  if (max_index < 0)
    return;
  const i64 max_vertex = max_index + (indexed ? base_vertex : 0);
  for (u32 i = 0; i < decl->vertexElementCount; ++i) {
    const auto& e = decl->vertexElements[i];
    const u32 stream = u32(e.stream);
    const u32 bytes = VertexFormatBytes(u32(e.type));
    if (stream >= kBoundStreamCount || !bytes)
      continue;
    const BoundStreamInfo vb = BoundStreamAt(stream);
    if (!vb.buffer || !vb.stride)
      continue;
    const u64 bound = vb.buffer->dataSize > vb.offset ? vb.buffer->dataSize - vb.offset : 0;
    const u64 need = u64(std::max<i64>(max_vertex, 0)) * vb.stride + u32(e.offset) + bytes;
    if (max_vertex >= 0 && need <= bound)
      continue;
    if (reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
      MGRN_WARN("[vb-bounds] present {} {} start={} count={} base={} max_vertex={} s{} "
                "u{}[{}] off={} stride={} needs {} bytes, bound {} (va 0x{:08X}+{})",
                present, indexed ? "idx" : "vtx", start, count, base_vertex, max_vertex,
                stream, u32(e.usage), u32(e.usageIndex), u32(e.offset), vb.stride, need,
                bound, vb.buffer->guestMirrorVa, vb.offset);
    return;
  }
}

void mgrn::native::CheckSkinSanity(u32 device_guest, const gpu::GuestVertexDeclaration* decl,
                                   bool indexed, u32 start, u32 count, i32 base_vertex,
                                   u32 palette_first_reg, u32 palette_regs, u32 present,
                                   u64 vs_hash) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  // UBYTE4N weights sum to 255 give or take rounding; bone rows hold rotation
  // (|v| <= ~1 times scale) and a translation in metres.
  constexpr i32 kWeightSumSlack = 4;
  constexpr float kMaxBoneComponent = 1000.0f;
  constexpr u32 kMaxReports = 100;
  static std::atomic<u32> reported{0};
  if (reported.load(std::memory_order_relaxed) >= kMaxReports)
    return;
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  if (!dev || !decl || !decl->vertexElements)
    return;
  i32 idx_off = -1, wgt_off = -1, pos_off = -1;
  for (u32 i = 0; i < decl->vertexElementCount; ++i) {
    const auto& e = decl->vertexElements[i];
    if (u32(e.stream) != 0 || e.usageIndex != 0)
      continue;
    const auto usage = static_cast<gpu::D3DDeclUsage>(e.usage);
    const auto type = static_cast<gpu::D3DDeclType>(u32(e.type));
    if (usage == gpu::D3DDeclUsage::kBlendIndices)
      idx_off = i32(u32(e.offset));
    else if (usage == gpu::D3DDeclUsage::kBlendWeight && type == gpu::D3DDeclType::kUByte4N)
      wgt_off = i32(u32(e.offset));
    else if (usage == gpu::D3DDeclUsage::kPosition && type == gpu::D3DDeclType::kFloat3)
      pos_off = i32(u32(e.offset));
  }
  const BoundStreamInfo vb = BoundStreamAt(0);
  if (idx_off < 0 || wgt_off < 0 || !vb.buffer || !vb.buffer->guestMirrorVa || !vb.stride)
    return;
  const auto* vdata = mgrn::mem::try_at<const u8>(vb.buffer->guestMirrorVa + vb.offset);
  const u32 vsize = vb.buffer->dataSize > vb.offset ? vb.buffer->dataSize - vb.offset : 0;
  const u8* idata = nullptr;
  u32 ielem = 2;
  if (indexed) {
    if (!indices || !indices->guestMirrorVa)
      return;
    idata = mgrn::mem::try_at<const u8>(indices->guestMirrorVa);
    ielem = indices->elementSize == 4 ? 4 : 2;
  }
  if (!vdata || (indexed && !idata))
    return;
  const u32 bone_count = palette_regs / 3;
  std::vector<bool> bone_checked(bone_count, false);
  // The host copy the GPU will read, to compare with the guest bytes.
  const u8* host_vdata = nullptr;
  if (vb.buffer->buffer && !vb.buffer->blockBuffer)
    if (auto* mapped = static_cast<const u8*>(vb.buffer->buffer->map()))
      host_vdata = mapped + vb.offset;
  u32 zero_guest = 0, zero_host = 0, host_diff = 0;
  // Bad: any non-finite or huge component, or an all-zero matrix (the vertex
  // collapses to the origin).
  auto bone_bad = [&](u32 bone, float& bad_value) {
    bool all_zero = true;
    for (u32 r = 0; r < 3; ++r)
      for (u32 c = 0; c < 4; ++c) {
        const float v = float(dev->vsFloatConstants[palette_first_reg + bone * 3 + r][c]);
        if (!std::isfinite(v) || std::fabs(v) > kMaxBoneComponent) {
          bad_value = v;
          return true;
        }
        all_zero = all_zero && v == 0.0f;
      }
    bad_value = 0.0f;
    return all_zero;
  };
  for (u32 n = 0; n < count; ++n) {
    i64 v = i64(start) + n;
    if (indexed) {
      const u64 at = u64(start + n) * ielem;
      if (at + ielem > indices->dataSize)
        break;
      v = ielem == 4 ? i64(u32(idata[at]) << 24 | u32(idata[at + 1]) << 16 |
                           u32(idata[at + 2]) << 8 | idata[at + 3])
                     : i64(u32(idata[at]) << 8 | idata[at + 1]);
      if (v == (ielem == 4 ? 0xFFFFFFFFll : 0xFFFFll))
        continue;
      v += base_vertex;
    }
    if (v < 0 || u64(v) * vb.stride + vb.stride > vsize)
      continue;
    const u8* vtx = vdata + u64(v) * vb.stride;
    if (pos_off >= 0) {
      const u8* gp = vtx + pos_off;
      bool guest_zero = true;
      for (u32 b = 0; b < 12; ++b)
        guest_zero = guest_zero && gp[b] == 0;
      zero_guest += guest_zero ? 1 : 0;
      if (host_vdata) {
        const u8* hp = host_vdata + u64(v) * vb.stride + pos_off;
        bool host_zero = true;
        for (u32 b = 0; b < 12; ++b)
          host_zero = host_zero && hp[b] == 0;
        zero_host += host_zero ? 1 : 0;
        // Host bytes are the guest's byte-swapped per dword.
        for (u32 d = 0; d < 3; ++d)
          if (hp[d * 4] != gp[d * 4 + 3] || hp[d * 4 + 3] != gp[d * 4]) {
            ++host_diff;
            break;
          }
      }
    }
    i32 weight_sum = 0;
    for (u32 lane = 0; lane < 4; ++lane) {
      const u32 weight = vtx[wgt_off + 3 - lane];
      const u32 bone = vtx[idx_off + 3 - lane];
      weight_sum += i32(weight);
      if (!weight || bone >= bone_count || bone_checked[bone])
        continue;
      bone_checked[bone] = true;
      float bad_value = 0.0f;
      if (bone_bad(bone, bad_value) &&
          reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
        MGRN_WARN("[skin-sanity] present {} bone {} (c{}) bad, value {} (vertex {}, vb "
                  "0x{:08X}, vs {:016X})",
                  present, bone, palette_first_reg + bone * 3, bad_value, v,
                  vb.buffer->guestMirrorVa, vs_hash);
    }
    if (std::abs(weight_sum - 255) > kWeightSumSlack &&
        reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
      MGRN_WARN("[skin-sanity] present {} vertex {} weights sum {} (vb 0x{:08X} stride {})",
                present, v, weight_sum, vb.buffer->guestMirrorVa, vb.stride);
  }
  if (host_vdata)
    vb.buffer->buffer->unmap();
  // Static meshes have no vertex at the exact origin; report any, and any
  // vertex whose host copy differs from the guest bytes.
  if ((zero_guest || zero_host || host_diff) &&
      reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
    MGRN_WARN("[skin-sanity] present {} vb 0x{:08X} vs {:016X}: zero positions guest {} host {}, "
              "host differs at {} vertices",
              present, vb.buffer->guestMirrorVa, vs_hash, zero_guest, zero_host, host_diff);
}

void mgrn::native::CheckIndexStability(bool indexed, u32 start, u32 count, u32 present) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  struct Seen {
    u64 hash = 0;
    u32 zeros = 0;
    u32 present = 0;
  };
  static std::unordered_map<u64, Seen> seen;
  static std::atomic<u32> reported{0};
  constexpr u32 kMaxReports = 200;
  // Small draws (UI and particle quads) cycle through rings by design.
  constexpr u32 kMinIndexCount = 64;
  if (!indexed || !indices || !indices->guestMirrorVa || count < kMinIndexCount ||
      reported.load(std::memory_order_relaxed) >= kMaxReports)
    return;
  const u32 elem = indices->elementSize == 4 ? 4 : 2;
  const u64 begin = u64(start) * elem, bytes = u64(count) * elem;
  if (begin + bytes > indices->dataSize)
    return;
  const auto* data = mgrn::mem::try_at<const u8>(indices->guestMirrorVa + u32(begin));
  if (!data)
    return;
  u32 zeros = 0;
  for (u64 i = 0; i < bytes; i += elem) {
    bool zero = true;
    for (u32 b = 0; b < elem; ++b)
      zero = zero && data[i + b] == 0;
    zeros += zero ? 1 : 0;
  }
  const u64 key = (u64(indices->guestMirrorVa + u32(begin)) << 24) ^ count;
  const u64 hash = XXH3_64bits(data, bytes);
  auto [it, inserted] = seen.try_emplace(key, Seen{hash, zeros, present});
  if (!inserted && it->second.hash != hash && it->second.present != present &&
      reported.fetch_add(1, std::memory_order_relaxed) < kMaxReports)
    MGRN_WARN("[ib-change] present {} ib 0x{:08X}+{} count={} changed since present {}: "
              "zeros {} -> {}",
              present, indices->guestMirrorVa, u32(begin), count, it->second.present,
              it->second.zeros, zeros);
  it->second = Seen{hash, zeros, present};
}

std::string mgrn::native::CheckSkinReferences(u32 device_guest,
                                              const gpu::GuestVertexDeclaration* decl,
                                              bool indexed, u32 start, u32 count,
                                              i32 base_vertex, u32 palette_first_reg,
                                              u32 palette_regs) {
  const gpu::GuestBuffer* indices = BoundIndexBuffer();
  const auto* dev = mgrn::mem::at<const gpu::D3DDevice>(device_guest);
  if (!dev || !decl || !decl->vertexElements)
    return " no device/decl";
  // Offsets of the stream-0 elements MGR's skinned decls carry.
  i32 pos_off = -1, idx_off = -1, wgt_off = -1;
  for (u32 i = 0; i < decl->vertexElementCount; ++i) {
    const auto& e = decl->vertexElements[i];
    if (u32(e.stream) != 0 || e.usageIndex != 0)
      continue;
    const auto usage = static_cast<gpu::D3DDeclUsage>(e.usage);
    const auto type = static_cast<gpu::D3DDeclType>(u32(e.type));
    if (usage == gpu::D3DDeclUsage::kPosition && type == gpu::D3DDeclType::kFloat3)
      pos_off = i32(u32(e.offset));
    else if (usage == gpu::D3DDeclUsage::kBlendIndices)
      idx_off = i32(u32(e.offset));
    else if (usage == gpu::D3DDeclUsage::kBlendWeight && type == gpu::D3DDeclType::kUByte4N)
      wgt_off = i32(u32(e.offset));
  }
  const BoundStreamInfo vb = BoundStreamAt(0);
  if (idx_off < 0 || !vb.buffer || !vb.buffer->guestMirrorVa || !vb.stride)
    return std::format(" skipped idx_off={} stride={}", idx_off, vb.stride);
  const auto* vdata = mgrn::mem::try_at<const u8>(vb.buffer->guestMirrorVa + vb.offset);
  const u32 vsize = vb.buffer->dataSize > vb.offset ? vb.buffer->dataSize - vb.offset : 0;
  const u8* idata = nullptr;
  u32 isize = 0, ielem = 0;
  if (indexed && indices && indices->guestMirrorVa) {
    idata = mgrn::mem::try_at<const u8>(indices->guestMirrorVa);
    isize = indices->dataSize;
    ielem = indices->elementSize == 4 ? 4 : 2;
  }
  if (!vdata || (indexed && !idata))
    return " unreadable";

  auto bone_is_zero = [&](u32 bone) {
    for (u32 r = palette_first_reg + bone * 3; r < palette_first_reg + bone * 3 + 3; ++r)
      for (u32 c = 0; c < 4; ++c)
        if (float(dev->vsFloatConstants[r][c]) != 0.0f)
          return false;
    return true;
  };
  const u32 bone_count = palette_regs / 3;
  u32 checked = 0, restarts = 0, out_of_range = 0, zero_bone = 0, oob_vertex = 0, max_index = 0;
  u32 first_bad_vertex = ~0u, first_bad_bone = 0;
  float max_pos = 0.0f;
  for (u32 n = 0; n < count; ++n) {
    i64 v = indexed ? 0 : i64(start) + n;
    if (indexed) {
      const u64 at = u64(start + n) * ielem;
      if (at + ielem > isize)
        break;
      v = ielem == 4 ? i64(u32(idata[at]) << 24 | u32(idata[at + 1]) << 16 |
                           u32(idata[at + 2]) << 8 | idata[at + 3])
                     : i64(u32(idata[at]) << 8 | idata[at + 1]);
      // Strip restart marker, not a vertex.
      if (v == (ielem == 4 ? 0xFFFFFFFFll : 0xFFFFll)) {
        ++restarts;
        continue;
      }
      v += base_vertex;
    }
    if (v < 0 || u64(v) * vb.stride + vb.stride > vsize) {
      ++oob_vertex;
      continue;
    }
    const u8* vtx = vdata + u64(v) * vb.stride;
    ++checked;
    // Guest big-endian dword: lane x is the last byte in memory.
    for (u32 lane = 0; lane < 4; ++lane) {
      const u32 bone = vtx[idx_off + 3 - lane];
      const u32 weight = wgt_off >= 0 ? vtx[wgt_off + 3 - lane] : 255;
      if (!weight)
        continue;
      max_index = std::max(max_index, bone);
      const bool bad_range = bone >= bone_count;
      const bool bad_zero = !bad_range && bone_is_zero(bone);
      out_of_range += bad_range;
      zero_bone += bad_zero;
      if ((bad_range || bad_zero) && first_bad_vertex == ~0u) {
        first_bad_vertex = u32(v);
        first_bad_bone = bone;
      }
    }
    if (pos_off >= 0)
      for (u32 c = 0; c < 3; ++c) {
        const u8* p = vtx + pos_off + c * 4;
        const u32 bits = u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3];
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        if (!(std::fabs(f) <= max_pos))
          max_pos = std::isfinite(f) ? std::fabs(f) : INFINITY;
      }
  }
  return std::format(" verts={} restarts={} max_bone={} past_palette={} zero_bone={} oob_vertex={} "
                     "first_bad=v{}/b{} max_pos={:.3f}",
                     checked, restarts, max_index, out_of_range, zero_bone, oob_vertex,
                     i32(first_bad_vertex), first_bad_bone, max_pos);
}

