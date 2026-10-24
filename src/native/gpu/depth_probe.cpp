/**
 * @file    gpu/depth_probe.cpp
 * @brief   MGR dev: depth target readback for the draw-dump frame.
 */
#include "gpu/depth_probe.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/resources.h"

namespace mgrn::gpu {

namespace {

constexpr u32 kMaxDepthProbes = 16;
constexpr u32 kDepthBytes = 4;      // D32 depth aspect, tightly packed texels
constexpr u32 kRowPixelAlign = 64;  // 256-byte rows

struct DepthProbe {
  std::unique_ptr<plume::RenderBuffer> readback;
  u64 size = 0;
  u32 width = 0;
  u32 height = 0;
  u32 row_pixels = 0;
  std::string tag;
};

struct DepthProbeState {
  std::array<DepthProbe, kMaxDepthProbes> probes;
  u32 recorded = 0;
};

DepthProbeState &probes() {
  static DepthProbeState p;
  return p;
}

bool IsReadableDepth(plume::RenderFormat format) {
  return format == plume::RenderFormat::D32_FLOAT ||
         format == plume::RenderFormat::D32_FLOAT_S8_UINT;
}

void LogProbe(const DepthProbe &probe, const u8 *mapped) {
  float lo = 1e30f, hi = -1e30f;
  double sum = 0.0;
  u64 zeros = 0, ones = 0;
  const u64 count = u64(probe.width) * probe.height;
  for (u32 y = 0; y < probe.height; ++y) {
    const u8 *row = mapped + size_t(y) * probe.row_pixels * kDepthBytes;
    for (u32 x = 0; x < probe.width; ++x) {
      float d;
      std::memcpy(&d, row + size_t(x) * kDepthBytes, sizeof(d));
      lo = std::min(lo, d);
      hi = std::max(hi, d);
      sum += d;
      zeros += (d == 0.0f);
      ones += (d == 1.0f);
    }
  }
  const auto sample = [&](u32 x, u32 y) {
    float d;
    std::memcpy(&d, mapped + (size_t(y) * probe.row_pixels + x) * kDepthBytes, sizeof(d));
    return d;
  };
  MGRN_WARN("[depth-probe] {} {}x{} min={:.6f} max={:.6f} mean={:.6f} zero={:.1f}% one={:.1f}% "
            "center={:.6f} left-third={:.6f} right-third={:.6f}",
            probe.tag, probe.width, probe.height, lo, hi, sum / double(count),
            100.0 * double(zeros) / double(count), 100.0 * double(ones) / double(count),
            sample(probe.width / 2, probe.height / 2), sample(probe.width / 6, probe.height / 2),
            sample(probe.width * 5 / 6, probe.height / 2));
}

} // namespace

void RecordDepthProbeLocked(VideoState &s, GuestTexture *ds, const char *tag) {
  auto &p = probes();
  if (p.recorded >= kMaxDepthProbes || !ds || !ds->texture || !s.command_list_open)
    return;
  if (!IsReadableDepth(ds->format) || ds->sampleCount != plume::RenderSampleCount::COUNT_1) {
    MGRN_WARN("[depth-probe] {} skipped: format {} samples {}", tag, u32(ds->format),
              u32(ds->sampleCount));
    return;
  }
  DepthProbe &probe = p.probes[p.recorded];
  const u32 row_pixels = (ds->width + kRowPixelAlign - 1) / kRowPixelAlign * kRowPixelAlign;
  const u64 size = u64(row_pixels) * ds->height * kDepthBytes;
  if (!probe.readback || probe.size < size) {
    probe.readback = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(size),
                                      "depth-probe-readback");
    probe.size = probe.readback ? size : 0;
  }
  if (!probe.readback)
    return;

  const auto previous = ds->layout;
  s.command_list->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(ds->texture, plume::RenderTextureLayout::COPY_SOURCE));
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(probe.readback.get(),
                                                        plume::RenderFormat::D32_FLOAT, ds->width,
                                                        ds->height, 1, row_pixels),
      plume::RenderTextureCopyLocation::Subresource(ds->texture, 0, 0));
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(ds->texture, previous == plume::RenderTextureLayout::UNKNOWN
                                                   ? plume::RenderTextureLayout::DEPTH_WRITE
                                                   : previous));
  ds->layout = previous == plume::RenderTextureLayout::UNKNOWN
                   ? plume::RenderTextureLayout::DEPTH_WRITE
                   : previous;
  probe.width = ds->width;
  probe.height = ds->height;
  probe.row_pixels = row_pixels;
  probe.tag = tag;
  ++p.recorded;
}

void FinishDepthProbes(VideoState &s, u32 frame_slot) {
  auto &p = probes();
  if (!p.recorded)
    return;
  // plume resets the fence after the wait; FinishCapture skips its own wait
  // once command_list_submitted is cleared.
  s.queue->waitForCommandFence(s.fences[frame_slot].get());
  s.command_list_submitted[frame_slot] = false;
  for (u32 i = 0; i < p.recorded; ++i) {
    const DepthProbe &probe = p.probes[i];
    const auto *mapped = static_cast<const u8 *>(probe.readback->map());
    if (!mapped) {
      MGRN_WARN("[depth-probe] {} map failed", probe.tag);
      continue;
    }
    LogProbe(probe, mapped);
    probe.readback->unmap();
  }
  p.recorded = 0;
}

} // namespace mgrn::gpu
