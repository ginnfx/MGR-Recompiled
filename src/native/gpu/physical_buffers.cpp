/**
 * @file    gpu/physical_buffers.cpp
 * @brief   Real geometry bypasses D3DDevice_CreateVertexBuffer. The
 *          bdPhysical*BufferCreate and bdSceneGraphRegister midasm feeders
 *          wrap already-loaded physical memory into an
 *          IDirect3DVertexBuffer9-layout struct, so the registry keys host
 *          GuestBuffers by struct VA for SetStreamSource and SetIndices to
 *          resolve.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/physical_buffers.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/thread/mutex.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/byte_swap.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/guest_write_watch.h"

// Block mirroring: one host RenderBuffer per model load (XPhysical) block with
// each mesh bound as an offset view, collapsing a model's thousands of tiny
// VB/IB allocations into one buffer. Physical buffers outside any block
// (UI/effects) keep the per-buffer path.

namespace {

struct PhysicalBufferRecord {
  std::unique_ptr<mgrn::gpu::GuestBuffer> buffer;
};

std::mutex g_physicalBuffersMutex;
std::unordered_map<u32, PhysicalBufferRecord> g_physicalBuffers;
// Bridges struct VA -> GuestBuffer owned by g_physicalBuffers. SetIndices and
// SetStreamSource get the struct VA, never a HostResourceHeap::Alloc'd one, so
// FromGuest misses. ResolveGuestBufferVa re-adopts a struct the engine has
// pointed at other memory since.
std::unordered_map<u32, mgrn::gpu::GuestBuffer *> g_physicalBufferStructs;

// Parked rather than freed inline: a prior frame's command list may still
// reference them. DrainBufferGraveyard frees them post-fence.
std::vector<std::unique_ptr<plume::RenderBuffer>>
    g_physicalBufferGraveyard[mgrn::gpu::kNumFrames];

// One entry per live XPhysical geometry block, living exactly as long as the
// model. Each owns one host RenderBuffer plus the per-mesh views into it, keyed
// by byte offset from the block base. Ordered so a draw-time base_va resolves
// by interval lookup. Guarded by g_physicalBuffersMutex.
struct PhysicalBlock {
  u32 base = 0;
  u32 size = 0;
  std::unique_ptr<plume::RenderBuffer>
      buffer; // lazy, VERTEX|INDEX, GeometryHeapType
  std::unordered_map<u32, std::unique_ptr<mgrn::gpu::GuestBuffer>> meshes;
};
std::map<u32 /*block base*/, PhysicalBlock> g_physicalBlocks;

mgrn::gpu::GuestBufferEvictedFn g_evictedListener = nullptr;

// Advanced once per drained frame slot. Per-buffer records idle this long are
// evicted: the engine re-points per-frame headers at new memory and frees
// effect/UI buffers, and nothing else ever removes those records.
u32 g_registryTick = 0;
constexpr u32 kIdleEvictTicks = 600;
constexpr u32 kIdleSweepInterval = 64;

// Live host geometry buffers owned by g_physicalBuffers, both guarded by
// g_physicalBuffersMutex. Tracked so UPLOAD heap growth is visible.
u32 g_physLiveCount = 0;
u64 g_physUploadBytes = 0;
// A runaway must show in the log as a rising plateau, not just as a later
// out-of-memory crash. Never decreases, so steady-state churn stays quiet.
constexpr u64 kPhysUploadWatermarkStep = 128ull << 20;
u64 g_physUploadWatermark = kPhysUploadWatermarkStep;

// Caller holds g_physicalBuffersMutex. Logs once per 128 MiB past the peak.
void NotePhysicalGrowth() {
  if (g_physUploadBytes <= g_physUploadWatermark)
    return;
  g_physUploadWatermark = ((g_physUploadBytes / kPhysUploadWatermarkStep) + 1) *
                          kPhysUploadWatermarkStep;
  MGRN_WARN("Physical geometry footprint high-water: {} MiB across {} buffers",
          g_physUploadBytes >> 20, g_physLiveCount);
}

// INDEX16 must swap per index: a bswap32 over INDEX16 swaps adjacent index
// pairs at every 4-byte boundary, giving out-of-range indices. VBs bswap32 and
// undo the 16-bit pair reorder shader-side (swapFloats swap mask).
std::unique_ptr<plume::RenderBuffer>
CreatePhysicalPlumeBuffer(plume::RenderDevice *device, u32 base_va, u32 size,
                          bool is_index, u32 element_size) {
  const plume::RenderHeapType heap =
      mgrn::gpu::GeometryHeapType(device, mgrn::gpu::GeometryClass::Static);
  plume::RenderBufferDesc desc =
      is_index ? plume::RenderBufferDesc::IndexBuffer(size, heap)
               : plume::RenderBufferDesc::VertexBuffer(size, heap);
  // plume's createBuffer is false-safe (non-null wrapper, null d3d) and the
  // map() below would deref that. Callers handle a null return: serve-stale on
  // refresh, skip on first-create.
  auto plume_buffer = mgrn::gpu::CreateHostBuffer(
      device, desc, is_index ? "physical-ib" : "physical-vb");
  if (!plume_buffer) {
    MGRN_ERROR(
        "CreatePhysicalPlumeBuffer: failed (size={}, {}), live={} uploadMiB={}",
        size, is_index ? "index" : "vertex", g_physLiveCount,
        g_physUploadBytes >> 20);
    return nullptr;
  }
  const auto *src8 = mgrn::mem::at<const u8>(base_va);
  if (!src8) {
    MGRN_WARN("CreatePhysicalPlumeBuffer: base_va 0x{:08X} did not translate",
            base_va);
    return nullptr;
  }
  void *mapped = plume_buffer->map();
  if (!mapped) {
    MGRN_ERROR("CreatePhysicalPlumeBuffer: map failed (size={})", size);
    return nullptr;
  }
  if (is_index)
    mgrn::gpu::ByteSwapIndexElements(mapped, src8, size, element_size);
  else
    mgrn::gpu::ByteSwapElements(mapped, src8, size, element_size);
  plume_buffer->unmap();
  return plume_buffer;
}

// Arms the write watch, then copies the guest bytes, so a write landing after
// the copy is always seen by the next check.
std::unique_ptr<plume::RenderBuffer>
CopyWatchedPhysicalBuffer(plume::RenderDevice *device, u32 base_va, u32 size,
                          bool is_index, u32 element_size, u32 &stamp) {
  constexpr u32 kStableCopyAttempts = 4;
  bool watched = false;
  for (u32 attempt = 0; attempt < kStableCopyAttempts; ++attempt) {
    // Guest physical-memory write callbacks take this same recursive mutex.
    // Arm while holding it: a guest write either completed before the watch
    // was armed, or faults and waits until this complete byte-swapped snapshot
    // is finished. Without this lock, a callback can unprotect a page midway
    // through the copy and leave half old, half new vertex/index data.
    auto guest_memory_lock = rex::thread::global_critical_region::AcquireDirect();
    stamp = mgrn::gpu::write_watch::Arm(base_va, size);
    watched = stamp != mgrn::gpu::write_watch::kUnwatched;
    if (!watched) {
      guest_memory_lock.unlock();
      break;
    }

    // Physical geometry can be written by an emulated guest thread while the
    // render thread copies it. A copy spanning that write combines two mesh
    // states and can send otherwise valid indices to unrelated vertices. The
    // watch callback re-arms only the pages it observed being written; arm
    // again before retrying and accept the snapshot only if no page changed.
    auto copy = CreatePhysicalPlumeBuffer(device, base_va, size, is_index,
                                          element_size);
    if (!copy) {
      guest_memory_lock.unlock();
      return nullptr;
    }
    const bool changed_during_copy =
        mgrn::gpu::write_watch::WrittenSince(base_va, size, stamp);
    guest_memory_lock.unlock();
    if (!changed_during_copy) {
      static u64 s_retries = 0;
      if (attempt && mgrn::DevMode() && s_retries++ < 64)
        MGRN_INFO("[stable-copy] physical buffer {:#010x}+{:#x} stabilized after {} retries",
                  base_va, size, attempt);
      return copy;
    }
  }

  if (watched) {
    static u64 s_unstable = 0;
    if (mgrn::DevMode() && s_unstable++ < 64)
      MGRN_WARN("[stable-copy] physical buffer {:#010x}+{:#x} changed during all {} copy attempts; "
                "serving the previous copy when available",
                base_va, size, kStableCopyAttempts);
  } else {
    static u32 s_warned = 0; // guarded by g_physicalBuffersMutex
    if (s_warned++ < 8)
      MGRN_WARN("Physical buffer {:#010x}+{:#x} is outside the physical heaps; "
                "guest rewrites of it are not tracked",
                base_va, size);
    return CreatePhysicalPlumeBuffer(device, base_va, size, is_index,
                                     element_size);
  }
  return nullptr;
}

// Replaces the host copy of 'buf' with a fresh one of base_va/size, parking
// the old buffer until no frame in flight can read it. Keeps the stale copy if
// the new one cannot be made. Caller holds g_physicalBuffersMutex.
bool RefreshPhysicalBufferLocked(mgrn::gpu::GuestBuffer *buf, u32 base_va,
                                 u32 size, u32 element_size) {
  auto *device = mgrn::gpu::Video::HostDevice();
  if (!device)
    return false;
  const bool is_index = buf->type == mgrn::gpu::ResourceType::IndexBuffer;
  u32 stamp = 0;
  auto fresh = CopyWatchedPhysicalBuffer(device, base_va, size, is_index,
                                         element_size, stamp);
  if (!fresh) {
    MGRN_WARN("Physical buffer refresh failed for base_va {:#010x} (size {}), "
              "serving stale buffer",
              base_va, size);
    return false;
  }
  if (buf->buffer)
    g_physicalBufferGraveyard[mgrn::gpu::Video::RetireSlot("physical buffer")]
        .push_back(std::move(buf->buffer));
  g_physUploadBytes += size;
  g_physUploadBytes -= std::min<u64>(g_physUploadBytes, buf->dataSize);
  buf->buffer = std::move(fresh);
  buf->dataSize = size;
  buf->guestMirrorVa = base_va;
  buf->elementSize = element_size;
  buf->writeStamp = stamp;
  // Rewritten geometry costs a full re-copy, so keep its volume visible.
  static u64 s_refreshes = 0, s_refresh_bytes = 0; // guarded by the mutex
  s_refresh_bytes += size;
  if ((++s_refreshes & 1023u) == 0)
    MGRN_INFO("Physical buffer refreshes: {} ({} MiB re-copied)", s_refreshes,
              s_refresh_bytes >> 20);
  return true;
}

mgrn::gpu::GuestBuffer *RegisterPhysicalBufferOnce(u32 base_va, u32 size,
                                                 mgrn::gpu::ResourceType rtype,
                                                 u32 element_size = 4) {
  if (!base_va || !size)
    return nullptr;
  std::lock_guard lock(g_physicalBuffersMutex);
  auto *device = mgrn::gpu::Video::HostDevice();
  if (!device) {
    static u32 s_warned = 0; // guarded by g_physicalBuffersMutex
    if (s_warned++ < 8)
      MGRN_ERROR("RegisterPhysicalBufferOnce: HostDevice null (base_va {:#010x})",
               base_va);
    return nullptr;
  }
  const bool is_index = (rtype == mgrn::gpu::ResourceType::IndexBuffer);

  auto it = g_physicalBuffers.find(base_va);
  if (it != g_physicalBuffers.end()) {
    auto *cached = it->second.buffer.get();
    if (cached->dataSize == size && cached->type == rtype) {
      return cached; // the same mesh is still at this address
    }
    static u32 s_reshapes = 0; // guarded by g_physicalBuffersMutex
    if (s_reshapes++ < 64)
      MGRN_INFO("physical buffer {:#010x} reshaped: {} {} bytes (elem {}) -> {} {} bytes "
                "(elem {})",
                base_va, cached->type == mgrn::gpu::ResourceType::IndexBuffer ? "ib" : "vb",
                cached->dataSize, cached->elementSize, is_index ? "ib" : "vb", size,
                element_size);
    cached->type = rtype;
    if (RefreshPhysicalBufferLocked(cached, base_va, size, element_size))
      cached->format = plume::RenderFormat::UNKNOWN; // caller re-pins IB format
    return cached; // stale rather than nothing if the refresh failed
  }

  u32 stamp = 0;
  auto plume_buffer = CopyWatchedPhysicalBuffer(device, base_va, size, is_index,
                                                element_size, stamp);
  if (!plume_buffer)
    return nullptr;

  auto guest_buffer = std::make_unique<mgrn::gpu::GuestBuffer>(rtype);
  guest_buffer->buffer = std::move(plume_buffer);
  guest_buffer->guestMirrorVa = base_va;
  guest_buffer->dataSize = size;
  guest_buffer->format = plume::RenderFormat::UNKNOWN;
  guest_buffer->elementSize = element_size;
  guest_buffer->writeStamp = stamp;
  guest_buffer->lastUseTick = g_registryTick;

  mgrn::gpu::GuestBuffer *raw = guest_buffer.get();
  g_physicalBuffers[base_va] = PhysicalBufferRecord{std::move(guest_buffer)};
  ++g_physLiveCount;
  g_physUploadBytes += size;
  NotePhysicalGrowth();
  return raw;
}

// Overwriting a prior entry is intended: the engine reuses a struct slot when
// meshes are reloaded.
void RememberBufferStruct(u32 struct_va, mgrn::gpu::GuestBuffer *buf) {
  if (!struct_va || !buf)
    return;
  std::lock_guard lock(g_physicalBuffersMutex);
  g_physicalBufferStructs[struct_va] = buf;
}

// --- Block helpers. All require the caller to hold g_physicalBuffersMutex. ---

// The block whose [base, base+size) contains base_va, or null.
PhysicalBlock *FindBlockForBaseLocked(u32 base_va) {
  if (g_physicalBlocks.empty())
    return nullptr;
  // First block with base > base_va.
  auto it = g_physicalBlocks.upper_bound(base_va);
  if (it == g_physicalBlocks.begin())
    return nullptr;
  --it; // greatest base <= base_va
  PhysicalBlock &a = it->second;
  if (base_va < a.base || base_va >= a.base + a.size)
    return nullptr;
  return &a;
}

// bswap16 per index for INDEX16, else bswap32 per dword. The source is the
// pristine guest big-endian block, so re-running rewrites the same bytes
// rather than swapping in place. The block buffer must already exist.
void SwapBlockRegionLocked(PhysicalBlock &a, u32 offset, u32 size,
                           u32 element_size, bool is_index) {
  const auto *src8 = mgrn::mem::at<const u8>(a.base + offset);
  if (!src8) {
    MGRN_WARN("Block swap: base {:#010x}+{:#x} did not translate", a.base,
            offset);
    return;
  }
  void *mapped = a.buffer->map();
  if (!mapped) {
    MGRN_ERROR("Block swap: map failed (block {:#010x})", a.base);
    return;
  }
  auto *dst8 = static_cast<u8 *>(mapped) + offset;
  if (is_index)
    mgrn::gpu::ByteSwapIndexElements(dst8, src8, size, element_size);
  else
    mgrn::gpu::ByteSwapElements(dst8, src8, size, element_size);
  a.buffer->unmap();
}

// Creates the block host buffer on first use, VERTEX|INDEX so one resource
// serves both binds. Null on any failure, so the caller falls back to the
// per-buffer path. A cached mesh is already byte-swapped.
mgrn::gpu::GuestBuffer *GetOrCreateBlockMeshLocked(PhysicalBlock &a, u32 base_va,
                                                 u32 size,
                                                 mgrn::gpu::ResourceType rtype,
                                                 u32 element_size,
                                                 plume::RenderFormat format) {
  if (base_va < a.base)
    return nullptr;
  const u32 offset = base_va - a.base;
  if (offset > a.size || size > a.size - offset) {
    MGRN_WARN("Block mesh [{:#010x}+{:#x}] outside block [{:#010x}+{:#x}]",
            base_va, size, a.base, a.size);
    return nullptr;
  }
  if (auto it = a.meshes.find(offset); it != a.meshes.end()) {
    mgrn::gpu::GuestBuffer *gb = it->second.get();
    if (format != plume::RenderFormat::UNKNOWN)
      gb->format = format;
    return gb;
  }
  auto *device = mgrn::gpu::Video::HostDevice();
  if (!device)
    return nullptr;
  if (!a.buffer) {
    plume::RenderBufferDesc desc;
    desc.size = a.size;
    desc.heapType =
        mgrn::gpu::GeometryHeapType(device, mgrn::gpu::GeometryClass::Static);
    desc.flags =
        plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX;
    a.buffer = mgrn::gpu::CreateHostBuffer(device, desc, "physical-block");
    if (!a.buffer) {
      MGRN_ERROR("Block buffer create failed (block {:#010x}, {} KiB)", a.base,
               a.size >> 10);
      return nullptr;
    }
    g_physUploadBytes += a.size;
    ++g_physLiveCount;
    NotePhysicalGrowth();
  }
  SwapBlockRegionLocked(a, offset, size, element_size,
                       rtype == mgrn::gpu::ResourceType::IndexBuffer);

  auto gb = std::make_unique<mgrn::gpu::GuestBuffer>(rtype);
  gb->blockBuffer = a.buffer.get();
  gb->blockOffset = offset;
  // base_va so a read-only Lock returns base_va+off and reads the pristine
  // guest bytes. A mirror lock wants guest data, not the swapped host copy.
  gb->guestMirrorVa = base_va;
  gb->ownsMirror = false;
  gb->dataSize = size;
  gb->format = format;
  gb->elementSize = element_size;
  mgrn::gpu::GuestBuffer *raw = gb.get();
  a.meshes[offset] = std::move(gb);
  return raw;
}

// Also registers the view in the struct VA index, so draw and lock resolution
// are unchanged. Null when base_va is in no known block.
mgrn::gpu::GuestBuffer *TryRegisterBlockMesh(u32 struct_va, u32 base_va, u32 size,
                                           mgrn::gpu::ResourceType rtype,
                                           u32 element_size,
                                           plume::RenderFormat format) {
  if (!base_va || !size)
    return nullptr;
  std::lock_guard lock(g_physicalBuffersMutex);
  PhysicalBlock *a = FindBlockForBaseLocked(base_va);
  if (!a)
    return nullptr;
  mgrn::gpu::GuestBuffer *view = GetOrCreateBlockMeshLocked(
      *a, base_va, size, rtype, element_size, format);
  if (!view)
    return nullptr;
  if (struct_va)
    g_physicalBufferStructs[struct_va] = view;
  return view;
}

// Every physical VB/IB creation site takes this route: block fast path first,
// then the per-buffer registry plus the struct VA bridge.
mgrn::gpu::GuestBuffer *RegisterPhysicalGeometry(u32 struct_va, u32 base_va,
                                               u32 size,
                                               mgrn::gpu::ResourceType rtype,
                                               u32 element_size,
                                               plume::RenderFormat format) {
  if (auto *view = TryRegisterBlockMesh(struct_va, base_va, size, rtype,
                                        element_size, format)) {
    return view;
  }
  auto *buf = RegisterPhysicalBufferOnce(base_va, size, rtype, element_size);
  if (buf && format != plume::RenderFormat::UNKNOWN)
    buf->format = format;
  RememberBufferStruct(struct_va, buf);
  return buf;
}

// Drops every struct bridge to 'gb' and parks its plume buffer for 'slot'.
// Caller holds g_physicalBuffersMutex and destroys 'gb' after FinishEviction.
void RetireRecordLocked(mgrn::gpu::GuestBuffer *gb, u32 slot,
                        std::vector<plume::RenderBuffer *> &scrub) {
  for (auto sit = g_physicalBufferStructs.begin();
       sit != g_physicalBufferStructs.end();) {
    if (sit->second == gb)
      sit = g_physicalBufferStructs.erase(sit);
    else
      ++sit;
  }
  if (gb->buffer) {
    scrub.push_back(gb->buffer.get());
    g_physicalBufferGraveyard[slot].push_back(std::move(gb->buffer));
  }
}

// Outside the registry lock: ScrubBufferBindings takes s.mutex, and the
// listener drops raw pointers before 'dead' destroys the GuestBuffers.
void FinishEviction(std::vector<std::unique_ptr<mgrn::gpu::GuestBuffer>> &dead,
                    const std::vector<plume::RenderBuffer *> &scrub) {
  if (g_evictedListener)
    for (const auto &gb : dead)
      g_evictedListener(gb.get());
  for (auto *b : scrub)
    mgrn::gpu::Video::ScrubBufferBindings(b);
  dead.clear();
}

// Evicts per-buffer records that no struct maps to any more (the engine
// re-pointed or reused every header naming them) and that no bind or draw
// resolved for kIdleEvictTicks. Mapped records stay: a creation-time IB
// cannot be re-adopted from its struct.
void EvictIdlePhysicalBuffers() {
  std::vector<plume::RenderBuffer *> scrub;
  std::vector<std::unique_ptr<mgrn::gpu::GuestBuffer>> dead;
  {
    std::lock_guard lock(g_physicalBuffersMutex);
    std::unordered_set<const mgrn::gpu::GuestBuffer *> mapped;
    for (const auto &[struct_va, gb] : g_physicalBufferStructs)
      mapped.insert(gb);
    u32 slot = 0;
    u64 freed_bytes = 0;
    for (auto it = g_physicalBuffers.begin(); it != g_physicalBuffers.end();) {
      mgrn::gpu::GuestBuffer *gb = it->second.buffer.get();
      if (mapped.count(gb) ||
          g_registryTick - gb->lastUseTick < kIdleEvictTicks) {
        ++it;
        continue;
      }
      if (dead.empty())
        slot = mgrn::gpu::Video::RetireSlot("idle geometry");
      RetireRecordLocked(gb, slot, scrub);
      freed_bytes += gb->dataSize;
      dead.push_back(std::move(it->second.buffer));
      it = g_physicalBuffers.erase(it);
    }
    if (!dead.empty()) {
      g_physLiveCount -= std::min<u64>(g_physLiveCount, dead.size());
      g_physUploadBytes -= std::min<u64>(g_physUploadBytes, freed_bytes);
      MGRN_DEBUG("Physical buffers: evicted {} idle ({} KiB), live={} uploadMiB={}",
                 dead.size(), freed_bytes >> 10, g_physLiveCount,
                 g_physUploadBytes >> 20);
    }
  }
  FinishEviction(dead, scrub);
}

// The engine frees a model's whole VB/IB block at once, so the host mirrors
// must go with it or the UPLOAD heap grows without bound. Serialized against
// the render thread's draw-time lookups by the BD DrawStart/DrawEnd handshake.
// The guest block memory is the engine's, freed by the following
// XPhysicalFree.
void EvictPhysicalBuffersInBlock(u32 block_base, u32 block_size) {
  if (!block_base || !block_size)
    return;
  const u32 block_end = block_base + block_size;
  // Scrubbed after the registry lock drops, since ScrubBufferBindings takes
  // s.mutex. The GuestBuffers stay alive until the listener has dropped every
  // raw pointer to them.
  std::vector<plume::RenderBuffer *> scrub;
  std::vector<std::unique_ptr<mgrn::gpu::GuestBuffer>> dead;
  {
    std::lock_guard lock(g_physicalBuffersMutex);
    const u32 slot = mgrn::gpu::Video::RetireSlot("geometry block");
    u32 evicted = 0;
    u64 freed_bytes = 0;
    for (auto it = g_physicalBuffers.begin(); it != g_physicalBuffers.end();) {
      if (it->first < block_base || it->first >= block_end) {
        ++it;
        continue;
      }
      mgrn::gpu::GuestBuffer *gb = it->second.buffer.get();
      // Struct VA bridges are keyed by a malloc'd VA outside the block, so
      // RetireRecordLocked matches them by value, not range.
      RetireRecordLocked(gb, slot, scrub);
      freed_bytes += gb->dataSize;
      dead.push_back(std::move(it->second.buffer));
      it = g_physicalBuffers.erase(it);
      ++evicted;
    }
    // Exact base: the destroy hook passes the same obj+0x18 the build hook
    // registered.
    u32 block_buffers = 0, block_meshes = 0;
    if (auto ait = g_physicalBlocks.find(block_base);
        ait != g_physicalBlocks.end()) {
      PhysicalBlock &block = ait->second;
      block_meshes = static_cast<u32>(block.meshes.size());
      for (auto &[offset, view] : block.meshes)
        dead.push_back(std::move(view));
      if (block.buffer) {
        plume::RenderBuffer *block_buf = block.buffer.get();
        for (auto sit = g_physicalBufferStructs.begin();
             sit != g_physicalBufferStructs.end();) {
          if (sit->second && sit->second->blockBuffer == block_buf)
            sit = g_physicalBufferStructs.erase(sit);
          else
            ++sit;
        }
        freed_bytes += block.size;
        block_buffers = 1;
        scrub.push_back(block_buf);
        g_physicalBufferGraveyard[slot].push_back(std::move(block.buffer));
      }
      g_physicalBlocks.erase(ait);
    }

    const u32 freed_buffers = evicted + block_buffers;
    if (freed_buffers || block_meshes) {
      g_physLiveCount = g_physLiveCount >= freed_buffers
                            ? g_physLiveCount - freed_buffers
                            : 0;
      g_physUploadBytes = g_physUploadBytes >= freed_bytes
                              ? g_physUploadBytes - freed_bytes
                              : 0;
      MGRN_DEBUG("Physical block freed [{:#010x}+{:#x}]: {} per-mesh + {} block "
               "buffer(s)"
               " ({} mesh views, {} KiB), live={} uploadMiB={}",
               block_base, block_size, evicted, block_buffers, block_meshes,
               freed_bytes >> 10, g_physLiveCount, g_physUploadBytes >> 20);
    }
  } // release g_physicalBuffersMutex before taking s.mutex in the scrub
  FinishEviction(dead, scrub);
}

} // namespace

namespace mgrn::gpu {

// SetIndices / SetStreamSource pass the struct VA, never a HostResourceHeap-
// allocated one, so FromGuest misses.
mgrn::gpu::GuestBuffer *FindPhysicalBufferByStruct(u32 struct_va) {
  if (!struct_va)
    return nullptr;
  std::lock_guard lock(g_physicalBuffersMutex);
  auto it = g_physicalBufferStructs.find(struct_va);
  return (it != g_physicalBufferStructs.end()) ? it->second : nullptr;
}

// Rebuild a GuestBuffer from a fully initialized X360 D3DIndexBuffer /
// D3DVertexBuffer struct: asset-loaded meshes patched by an unhooked loader,
// which is most of the scene geometry. Xenos fetch constant encoding, preserved
// through XGOffsetResourceAddress:
//   struct[+0x18] = base_va | tag_bits
//   struct[+0x1C] = (size & 0x3FFFFFC) | 0x10000002
//   struct[+0x00] bit 31 = index format flag (1 = INDEX32, 0 = INDEX16)
namespace {

// What a D3DVertexBuffer / D3DIndexBuffer struct currently describes.
struct BufferStructDesc {
  u32 base_va = 0;
  u32 size = 0;
  u32 element_size = 4;
  plume::RenderFormat ib_format = plume::RenderFormat::UNKNOWN;
};

bool ReadBufferStruct(u32 struct_va, ResourceType rtype,
                      BufferStructDesc &out) {
  // Both structs share this layout, and only Common/FetchLo/FetchHi are read.
  const auto *raw = mgrn::mem::at<const D3DVertexBuffer>(struct_va);
  if (!raw)
    return false;
  // Two encodings (d3d.h). Post-XGOffset FetchLo holds a physical-heap VA
  // (>= 0xA0000000); the bdPhysical*Create replication puts the size there,
  // which the 26-bit size field caps below kPreXGOffsetLimit.
  constexpr u32 kPreXGOffsetLimit = 0x04000000u;
  const u32 fetch_lo = u32(raw->FetchLo), fetch_hi = u32(raw->FetchHi);
  if (fetch_lo < kPreXGOffsetLimit) {
    // The IB replication stores a masked base the data does not live at, so
    // only the creation-time registration knows it.
    if (rtype == ResourceType::IndexBuffer)
      return false;
    out.base_va = fetch_hi & 0xFFFFFFFCu;
    out.size = fetch_lo & 0x03FFFFFCu;
  } else {
    out.base_va = fetch_lo & 0xFFFFFFFCu;
    out.size = fetch_hi & 0x03FFFFFCu;
  }
  if (!out.base_va || !out.size)
    return false;
  // The IB bswap stride depends on the index format (bit 31 of dword[0] =
  // INDEX32), so read it first and swap per element instead of per dword.
  if (rtype == ResourceType::IndexBuffer) {
    const bool index32 = (raw->resource.Common & 0x80000000u) != 0;
    out.element_size = index32 ? 4u : 2u;
    out.ib_format =
        index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT;
  }
  return true;
}

GuestBuffer *AdoptBufferStruct(u32 struct_va, ResourceType rtype) {
  BufferStructDesc desc;
  if (!ReadBufferStruct(struct_va, rtype, desc))
    return nullptr;
  return RegisterPhysicalGeometry(struct_va, desc.base_va, desc.size, rtype,
                                  desc.element_size, desc.ib_format);
}

} // namespace

mgrn::gpu::GuestBuffer *
AdoptPhysicalBuffer(u32 struct_va, mgrn::gpu::ResourceType rtype) {
  if (!struct_va)
    return nullptr;
  if (auto *existing = FindPhysicalBufferByStruct(struct_va))
    return existing;
  return AdoptBufferStruct(struct_va, rtype);
}

bool RefreshIfWritten(GuestBuffer *buf) {
  if (!buf || buf->blockBuffer || !buf->buffer)
    return false;
  if (!write_watch::WrittenSince(buf->guestMirrorVa, buf->dataSize,
                                 buf->writeStamp))
    return false;
  std::lock_guard lock(g_physicalBuffersMutex);
  return RefreshPhysicalBufferLocked(buf, buf->guestMirrorVa, buf->dataSize,
                                     buf->elementSize);
}

namespace {

GuestBuffer *ResolveGuestBufferVaUnmarked(u32 va, ResourceType rtype) {
  auto *buf = FindPhysicalBufferByStruct(va);
  if (!buf)
    return AdoptBufferStruct(va, rtype);
  // The engine re-points a struct at other memory (per-frame buffers do).
  BufferStructDesc desc;
  if (ReadBufferStruct(va, rtype, desc) &&
      (desc.base_va != buf->guestMirrorVa || desc.size != buf->dataSize) &&
      !buf->blockBuffer)
    return AdoptBufferStruct(va, rtype);
  return buf;
}

} // namespace

GuestBuffer *ResolveGuestBufferVa(u32 va, ResourceType rtype) {
  if (!va)
    return nullptr;
  auto *buf = ResolveGuestBufferVaUnmarked(va, rtype);
  if (buf)
    buf->lastUseTick = g_registryTick;
  return buf;
}

void SetGuestBufferEvictedListener(GuestBufferEvictedFn fn) {
  g_evictedListener = fn;
}

void DrainBufferGraveyard(u32 slot) {
  std::vector<std::unique_ptr<plume::RenderBuffer>> dead;
  {
    std::lock_guard lock(g_physicalBuffersMutex);
    dead.swap(g_physicalBufferGraveyard[slot]);
  }
  // Outside the registry lock. The refresh path replaces the plume buffer
  // without touching the draw state views, and BeginCommandList force-dirties
  // every stream, so the next flush would re-bind through the freed object.
  for (auto &b : dead)
    mgrn::gpu::Video::ScrubBufferBindings(b.get());
  dead.clear();
  if ((++g_registryTick % kIdleSweepInterval) == 0)
    EvictIdlePhysicalBuffers();
}

} // namespace mgrn::gpu

namespace {

// The guest builds an IDirect3DVertexBuffer9 struct in out_buffer:
//   [0x00] (type << 29) | 2, plus 0x200000 if flags&4,
//          plus 0x400000 if flags&0x200
//   [0x14] 0xFFFF0000                               BaseFlush
//   [0x18] size                                     Size
//   [0x1C] base_va                                  BaseAddress
// The 16 bytes between +0x04 and +0x14 stay zeroed. Replicate that verbatim and
// register (base_va, size) so draw-time fetch decode resolves it.
u32 bdPhysicalVertexBufferCreate_hook(
    u32 base_va, u32 flags_short, u32 type_bits, u32 /*unused*/, u32 size,
    rex::MappedPtr<mgrn::gpu::D3DVertexBuffer> vb) {
  if (vb) {
    u32 common = 2u;
    if (flags_short & 0x004u)
      common = 0x200002u;
    if (flags_short & 0x200u)
      common |= 0x400000u;
    vb->resource.Common = (type_bits << 29) | common;
    vb->resource.ReferenceCount = 0u;
    vb->resource.Fence = 0u;
    vb->resource.ReadFence = 0u;
    vb->resource.Identifier = 0u;
    vb->resource.BaseFlush = 0xFFFF0000u;
    vb->FetchLo = size;
    vb->FetchHi = base_va;
  }
  RegisterPhysicalGeometry(vb.guest_address(), base_va, size,
                           mgrn::gpu::ResourceType::VertexBuffer,
                           /*element_size=*/4, plume::RenderFormat::UNKNOWN);
  return vb.guest_address();
}

// The guest builds an IDirect3DIndexBuffer9 struct in out_buffer:
//   [0x00] 1, plus 0x200000 if flags&4, plus 0x400000 if flags&0x200
//   [0x04] 1                                        ReferenceCount
//   [0x14] 0xFFFF0000                               BaseFlush
//   [0x18] size | 3                                 Size with low bits
//   [0x1C] (base_va & 0x3FFFFFC) | 0x10000002       BaseAddress + flags
// Register the raw pre-mask base_va so draw-time IB resolution finds it: the
// mask trims to physical memory bounds, but the data lives at the unmasked VA.
u32 bdPhysicalIndexBufferCreate_hook(
    u32 base_va, u32 flags_short, u32 /*type_bits*/, u32 size,
    rex::MappedPtr<mgrn::gpu::D3DIndexBuffer> ib) {
  if (ib) {
    u32 common = 1u;
    if (flags_short & 0x004u)
      common = 0x200001u;
    if (flags_short & 0x200u)
      common |= 0x400000u;
    ib->resource.Common = common;
    ib->resource.ReferenceCount = 1u;
    ib->resource.Fence = 0u;
    ib->resource.ReadFence = 0u;
    ib->resource.Identifier = 0u;
    ib->resource.BaseFlush = 0xFFFF0000u;
    ib->FetchLo = size | 3u;
    ib->FetchHi = (base_va & 0x3FFFFFCu) | 0x10000002u;
  }
  // Common bit 31 stays clear, so this is INDEX16: element_size=2 makes the
  // upload bswap16 per index instead of bswap32 per dword, which would swap
  // adjacent index pairs and corrupt strip IBs.
  RegisterPhysicalGeometry(ib.guest_address(), base_va, size,
                           mgrn::gpu::ResourceType::IndexBuffer,
                           /*element_size=*/2, plume::RenderFormat::R16_UINT);
  return ib.guest_address();
}

} // namespace

REX_HOOK(bdPhysicalVertexBufferCreate, bdPhysicalVertexBufferCreate_hook);
REX_HOOK(bdPhysicalIndexBufferCreate, bdPhysicalIndexBufferCreate_hook);

// Midasm hook bodies. The generated recompiler dispatch externs these by name,
// so neither the names nor the register order may change: see
// config/hooks/geometry.toml. INDEX16 needs bswap16 per index, not bswap32 per
// dword, which would swap adjacent index pairs at 4-byte boundaries.
void bdSceneGraphRegisterIBHook(PPCRegister &r3, PPCRegister &r4,
                                PPCRegister &r30) {
  const u32 struct_va = r3.u32;
  const u32 base_addr = r4.u32;
  const u32 size_bytes = r30.u32;
  if (!struct_va || !base_addr || !size_bytes)
    return;
  const auto *ib = mgrn::mem::at<const mgrn::gpu::D3DIndexBuffer>(struct_va);
  const bool index32 = ib && (ib->resource.Common & 0x80000000u) != 0;
  const plume::RenderFormat ib_format =
      index32 ? plume::RenderFormat::R32_UINT : plume::RenderFormat::R16_UINT;
  RegisterPhysicalGeometry(struct_va, base_addr, size_bytes,
                           mgrn::gpu::ResourceType::IndexBuffer,
                           index32 ? 4u : 2u, ib_format);
}

// VB midasm hook. Fires.
void bdSceneGraphRegisterVBHook(PPCRegister &r3, PPCRegister &r4,
                                PPCRegister &r31) {
  const u32 struct_va = r3.u32;
  const u32 base_addr = r4.u32;
  const u32 size_bytes = r31.u32;
  if (!struct_va || !base_addr || !size_bytes)
    return;
  RegisterPhysicalGeometry(struct_va, base_addr, size_bytes,
                           mgrn::gpu::ResourceType::VertexBuffer,
                           /*element_size=*/4, plume::RenderFormat::UNKNOWN);
}

// Only the two fields read here are named.
struct BdSceneGraphObject {
  u8 _pad00[0x04];
  // HDB header[1] copy taken at build. Block size = (hdbHeader1 >> 1) &
  // 0x0FFF0000, the exact size passed to XPhysicalAllocEx.
  be_u32 hdbHeader1;
  u8 _pad08[0x18 - 0x08];
  // XPhysicalAllocEx geometry block base, 0 = model has no geometry block.
  be_u32 physicalBlockBase;
};
static_assert(offsetof(BdSceneGraphObject, hdbHeader1) == 0x04);
static_assert(offsetof(BdSceneGraphObject, physicalBlockBase) == 0x18);

// Evicting at bdSceneGraphDestroy's XPhysicalFree ties host UPLOAD buffer
// lifetime to the guest geometry it mirrors, so a reused block cannot serve
// stale geometry.
void bdSceneGraphDestroyHook(PPCRegister &r3, PPCRegister &r31) {
  const u32 block_base = r3.u32;
  if (!block_base)
    return;
  const auto *obj = mgrn::mem::at<const BdSceneGraphObject>(r31.u32);
  if (!obj)
    return;
  const u32 block_size = (obj->hdbHeader1 >> 1) & 0x0FFF0000u;
  EvictPhysicalBuffersInBlock(block_base, block_size);
}

// After obj+0x18 receives the block base and before the meshes are
// bump-allocated into it, so the per-mesh feeders resolve base_va to this
// block. Registers [base, size) only, the host buffer waits for the first mesh.
// Reads the same two obj fields the destroy hook frees by, so the two agree.
void bdSceneGraphBuildHook(PPCRegister &r31) {
  const auto *obj = mgrn::mem::at<const BdSceneGraphObject>(r31.u32);
  if (!obj)
    return;
  const u32 block_base = obj->physicalBlockBase;
  if (!block_base)
    return;
  const u32 block_size = (obj->hdbHeader1 >> 1) & 0x0FFF0000u; // == build alloc
  if (!block_size)
    return;
  std::lock_guard lock(g_physicalBuffersMutex);
  PhysicalBlock &a = g_physicalBlocks[block_base];
  a.base = block_base;
  a.size = block_size;
}
