/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"
#include "gpu/capture.h"
#include "gpu/depth_probe.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "gpu/constant_buffers.h"
#include "gpu/output.h"
#include "gpu/settings.h"

namespace mgrn::gpu {

namespace {

void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Get().Vsync());
  }
}

constexpr i32 kIdleFPS = 30;

void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  i32 fps = mgrn::engine::Settings::Get().FPSLimit();
  if (mgrn::engine::SofdecPlayer::Playing())
    fps = 30;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    next += period;
  } else {
    next = now + period;
  }
}

void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

void RebuildSwapChain(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      MGRN_ERROR("Swap chain resize failed");
    return;
  }
  if (static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
      AspectMode::Auto)
    Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  MGRN_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  const double present_aspect =
      (mgrn::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  constexpr float kPresentGamma = 1.0f;
  // Match Xenos' uncorrected front-buffer scanout. The re:Blue TV LUT
  // reshaping is game-specific and is not part of MGR's native contract.
  constexpr float kPresentDisplayCorrection = 0.0f;
  s.command_list->setPipeline(s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, 0, s.guest_gamma * kPresentGamma,
       kPresentDisplayCorrection};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

namespace {

// Guest-pixel rectangles to host pixels of a target at 'scale'.
std::vector<plume::RenderRect>
ScaleClearRects(const std::vector<plume::RenderRect> &guest_rects, float scale) {
  std::vector<plume::RenderRect> host;
  host.reserve(guest_rects.size());
  for (const auto &r : guest_rects)
    host.emplace_back(i32(r.left * scale), i32(r.top * scale),
                      i32(r.right * scale), i32(r.bottom * scale));
  return host;
}

void ClearLocked(VideoState &s, u32 flags, plume::RenderColor color,
                 float depth, u32 stencil,
                 const std::vector<plume::RenderRect> &guest_rects) {
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!Video::BindDrawFramebufferLocked(clear_color))
    return;
  const GuestTexture *scaled = rt ? rt : ds;
  const auto rects = ScaleClearRects(guest_rects, scaled->renderScale);
  const plume::RenderRect *rect_data = rects.empty() ? nullptr : rects.data();
  const u32 rect_count = u32(rects.size());
  if (clear_color)
    s.command_list->clearColor(0, color, rect_data, rect_count);
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth,
                                      stencil, rect_data, rect_count);
  }
}

} // namespace

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil,
                  const std::vector<plume::RenderRect> &guest_rects) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ClearLocked(s, flags, ArgbToRenderColor(color_argb), depth, stencil,
              guest_rects);
}

void Video::Clear(u32 flags, plume::RenderColor color, float depth,
                  u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  ClearLocked(s, flags, color, depth, stencil, {});
}

void Video::ArmNextPresent() {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::Present(GuestTexture *frontBuffer) {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  u32 texture_index = 0;
  {
    MGRN_CPU_ZONE("AcquireTexture");
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
  }

  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      MGRN_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  BeginCommandList(s);
  RecordPresentPass(s, rt, back, back_fb);
  // Optional scanout capture includes the present ramp and sRGB view decode;
  // the default guest-buffer capture preserves internal-resolution texels.
  static const bool capture_present = std::getenv("MGR_NATIVE_CAPTURE_PRESENT") != nullptr;
  if (capture_present) {
    GuestTexture presented;
    presented.texture = back;
    presented.width = s.swap_chain->getWidth();
    presented.height = s.swap_chain->getHeight();
    presented.format = plume::RenderFormat::B8G8R8A8_UNORM;
    presented.layout = plume::RenderTextureLayout::PRESENT;
    RecordCaptureCopy(s, &presented);
  } else {
    RecordCaptureCopy(s, rt);
  }

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    MGRN_CPU_ZONE("Submit");
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  FinishDepthProbes(s, cur);  // MGR dev: draw-dump depth readback
  FinishCapture(s, cur);
  ApplyVsync(s);
  {
    MGRN_CPU_ZONE("PresentSwap");
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
  }
  {
    MGRN_CPU_ZONE("WaitFence");
    AdvanceAndWaitReused(s);
  }
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  MGRN_FRAME_MARK();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    MGRN_CPU_ZONE("DrainSlot");
    DrainSlot(s, reclaimed);
  }
  {
    MGRN_CPU_ZONE("PaceFrame");
    PaceFrame();
  }
}

void Video::SkipPresent() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        MGRN_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace mgrn::gpu
