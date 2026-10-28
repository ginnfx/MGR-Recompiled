/**
 * @file    gpu/capture.cpp
 * @brief   MGR: front buffer readback for the scripted test harness.
 */
#include "gpu/capture.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <string>
#include <memory>
#include <mutex>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/frame.h"
#include "gpu/resources.h"

namespace mgrn::gpu {

namespace {

constexpr u32 kRowAlignment = 256;

struct CaptureState {
  std::mutex mutex;
  std::condition_variable done;
  bool requested = false;
  bool recorded = false;
  bool ready = false;
  CapturedFrame frame;
  u32 row_bytes = 0;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  std::unique_ptr<plume::RenderBuffer> readback;
  u64 readback_size = 0;
  std::string path;
};

CaptureState &capture() {
  static CaptureState c;
  return c;
}

std::vector<std::unique_ptr<CaptureState>> draw_captures;

u32 BytesPerPixel(plume::RenderFormat format) {
  switch (format) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
  case plume::RenderFormat::R8G8B8A8_UNORM_SRGB:
  case plume::RenderFormat::B8G8R8A8_UNORM:
    return 4;
  case plume::RenderFormat::R16G16B16A16_FLOAT:
  case plume::RenderFormat::R16G16B16A16_UNORM:
    return 8;
  default:
    return 0;
  }
}

float HalfToFloat(u16 h) {
  const u32 sign = u32(h & 0x8000u) << 16;
  const u32 exponent = (h >> 10) & 0x1Fu;
  const u32 mantissa = h & 0x3FFu;
  u32 bits;
  if (exponent == 0) {
    bits = sign;  // denormals read as zero; fine for a capture
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

u8 ToByte(float v) {
  v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
  return static_cast<u8>(v * 255.0f + 0.5f);
}

void ConvertRow(plume::RenderFormat format, const u8 *src, u8 *dst, u32 width) {
  switch (format) {
  case plume::RenderFormat::B8G8R8A8_UNORM:
    for (u32 x = 0; x < width; ++x) {
      dst[4 * x + 0] = src[4 * x + 2];
      dst[4 * x + 1] = src[4 * x + 1];
      dst[4 * x + 2] = src[4 * x + 0];
      dst[4 * x + 3] = src[4 * x + 3];
    }
    break;
  case plume::RenderFormat::R16G16B16A16_FLOAT:
    for (u32 x = 0; x < width * 4; ++x) {
      u16 h;
      std::memcpy(&h, src + 2 * x, 2);
      dst[x] = ToByte(HalfToFloat(h));
    }
    break;
  case plume::RenderFormat::R16G16B16A16_UNORM:
    for (u32 x = 0; x < width * 4; ++x) {
      u16 v;
      std::memcpy(&v, src + 2 * x, 2);
      dst[x] = static_cast<u8>(v >> 8);
    }
    break;
  default:  // RGBA8, raw bytes (an sRGB format already holds encoded values)
    std::memcpy(dst, src, size_t(width) * 4);
    break;
  }
}

} // namespace

bool CaptureNextPresent(CapturedFrame &out, u32 timeout_ms) {
  auto &c = capture();
  std::unique_lock lock(c.mutex);
  c.requested = true;
  c.ready = false;
  const bool ok = c.done.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                  [&] { return c.ready; });
  c.requested = false;
  if (!ok)
    return false;
  out = std::move(c.frame);
  c.frame = {};
  c.ready = false;
  return out.width != 0;
}

static void RecordCopy(VideoState &s, GuestTexture *source, CaptureState &c) {
  if (!c.requested || c.recorded || !source || !source->texture)
    return;
  const u32 bpp = BytesPerPixel(source->format);
  if (!bpp) {
    MGRN_WARN("capture: present source format {} is not readable", u32(source->format));
    return;
  }
  const u32 width = source->width, height = source->height;
  const u32 row_bytes = (width * bpp + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
  const u64 size = u64(row_bytes) * height;
  if (!c.readback || c.readback_size < size) {
    c.readback = CreateHostBuffer(s.device.get(), plume::RenderBufferDesc::ReadbackBuffer(size),
                                  "capture-readback");
    c.readback_size = c.readback ? size : 0;
  }
  if (!c.readback)
    return;
  const auto previous = source->layout;
  s.command_list->barriers(
      plume::RenderBarrierStage::COPY,
      plume::RenderTextureBarrier(source->texture, plume::RenderTextureLayout::COPY_SOURCE));
  s.command_list->copyTextureRegion(
      plume::RenderTextureCopyLocation::PlacedFootprint(c.readback.get(), source->format, width,
                                                        height, 1, row_bytes / bpp),
      plume::RenderTextureCopyLocation::Subresource(source->texture, 0, 0));
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(source->texture, previous == plume::RenderTextureLayout::UNKNOWN
                                                       ? plume::RenderTextureLayout::SHADER_READ
                                                       : previous));
  c.frame.width = width;
  c.frame.height = height;
  c.row_bytes = row_bytes;
  c.format = source->format;
  c.recorded = true;
}

void RecordCaptureCopy(VideoState &s, GuestTexture *source) {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  RecordCopy(s, source, c);
}

void RecordDrawCapture(VideoState &s, GuestTexture *source, const std::string &path) {
  // One selected frame only; bound memory use even if the range is too broad.
  if (draw_captures.size() >= 32) return;
  auto c = std::make_unique<CaptureState>();
  c->requested = true;
  c->path = path;
  RecordCopy(s, source, *c);
  if (c->recorded) draw_captures.push_back(std::move(c));
}

void FinishCapture(VideoState &s, u32 frame_slot) {
  auto &c = capture();
  std::lock_guard lock(c.mutex);
  if (!draw_captures.empty()) {
    if (s.command_list_submitted[frame_slot])
      s.queue->waitForCommandFence(s.fences[frame_slot].get());
    s.command_list_submitted[frame_slot] = false;
    for (auto &d : draw_captures) {
      const auto *mapped = static_cast<const u8 *>(d->readback->map());
      if (!mapped) continue;
      std::ofstream file(d->path, std::ios::binary);
      file << "P6\n" << d->frame.width << " " << d->frame.height << "\n255\n";
      std::vector<u8> row(size_t(d->frame.width) * 4);
      for (u32 y = 0; y < d->frame.height; ++y) {
        ConvertRow(d->format, mapped + size_t(y) * d->row_bytes, row.data(), d->frame.width);
        for (u32 x = 0; x < d->frame.width; ++x)
          file.write(reinterpret_cast<const char *>(row.data() + x * 4), 3);
      }
      d->readback->unmap();
      MGRN_WARN("[draw-capture] {}", d->path);
    }
    draw_captures.clear();
  }
  if (!c.recorded)
    return;
  c.recorded = false;
  // plume resets the fence after the wait, so the frame ring must not wait on
  // this slot again when it comes round (nor here, after FinishDepthProbes).
  if (s.command_list_submitted[frame_slot])
    s.queue->waitForCommandFence(s.fences[frame_slot].get());
  s.command_list_submitted[frame_slot] = false;
  const auto *mapped = static_cast<const u8 *>(c.readback->map());
  if (!mapped) {
    c.frame = {};
  } else {
    c.frame.rgba.resize(size_t(c.frame.width) * c.frame.height * 4);
    for (u32 y = 0; y < c.frame.height; ++y)
      ConvertRow(c.format, mapped + size_t(y) * c.row_bytes,
                 c.frame.rgba.data() + size_t(y) * c.frame.width * 4, c.frame.width);
    c.readback->unmap();
  }
  c.ready = true;
  c.done.notify_all();
}

} // namespace mgrn::gpu
