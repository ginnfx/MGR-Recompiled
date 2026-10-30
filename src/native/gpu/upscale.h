/**
 * @file    gpu/upscale.h
 * @brief   Temporal upscaling of MGR's 3D scene (mgr_upscaler): sub-pixel
 *          jitter on scene draws, camera tracking, and the MetalFX pass
 *          between the scene's last resolve and the composite.
 *
 * MGR draws its 3D scene into a full-screen target (1280x720, or 1152x720 in
 * some areas), resolves the finished scene into a texture, and then draws the
 * composite (the scene texture plus bloom) and the HUD into a 1280x720 target,
 * often the same one the scene used. While upscaling, every target the game
 * binds renders at the input scale. At the composite's first draw the scene is
 * upscaled into a separate texture that the composite samples in place of the
 * scene texture, and the composite target is redirected for the rest of the
 * frame to a private target at the output scale.
 *
 * Every function runs on the render thread; the *Locked ones with
 * VideoState::mutex held.
 */
#pragma once

#include <rex/types.h>

#include "gpu/device.h"

namespace mgrn::gpu::upscale {

// Device init: brings up MetalFX and tells the settings what ratio it takes.
void InitDevice(VideoState &s);

// A draw is about to bind its targets: tracks the scene, and at the
// composite's first draw upscales it and redirects the composite target. This
// can submit the frame so far and open a new command list.
void BeforeDrawLocked(VideoState &s, u32 device_guest);

// Every resolve (lazy alias or copy) from 'src' into 'dst'.
void NoteResolveLocked(VideoState &s, GuestTexture *src, GuestTexture *dst);

// The clip-space offset (g_HalfPixelOffset) for the draw being flushed: the
// frame's jitter on depth-tested scene draws, else zero. The first
// depth-writing scene draw of a frame records the camera.
void SceneDrawOffsetLocked(VideoState &s, u32 device_guest, f32 &x, f32 &y);

// The pixel shaders' mip bias for the draw being flushed: log2(input/output)
// on draws into the scene targets while upscaling, so textures keep the detail
// of the output size; else zero.
f32 SceneMipBiasLocked(const VideoState &s);

// The upscaled scene when 'texture' is a scene texture it was made from, else
// nullptr.
GuestTexture *SubstituteLocked(const GuestTexture *texture);

// The target to bind for 'surface': the output-size composite target while
// the composite redirects 'surface', else 'surface'.
GuestTexture *RedirectTarget(GuestTexture *surface);

// 'dead' is being destroyed.
void ForgetTextureLocked(VideoState &s, GuestTexture *dead);

// Frame end: restores the redirected target, carries the camera over and
// advances the jitter.
void EndFrameLocked(VideoState &s);

} // namespace mgrn::gpu::upscale
