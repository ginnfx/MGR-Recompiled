/**
 * @file    gpu/upscale.cpp
 * @brief   Temporal upscaling of MGR's 3D scene; see upscale.h.
 */
#include "gpu/upscale.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/format.h"
#include "gpu/frame.h"
#include "gpu/metalfx.h"
#include "gpu/settings.h"
#include "gpu/upscale_math.h"

namespace mgrn::gpu::upscale {

namespace {

// MGR's shaders keep g_ViewProjection in VS c8-c11, one row per register:
// clip = dot(c8..c11, world position).
constexpr u32 kViewProjectionRegister = 8;
// RB_DEPTHCONTROL z-enable bit and RB_COLOR_MASK, read before the draw's
// state is flushed.
constexpr u32 kRegDepthControl = 0x2200, kDepthControlZEnable = 0x2u;
constexpr u32 kRegColorMask = 0x2104, kColorMaskAll = 0xFu;
// A screen point that moves more than this (in NDC, 2 = the screen width)
// between frames is a camera cut: the history is dropped.
constexpr f64 kCameraCutNdc = 0.5;
constexpr u64 kDevLogInterval = 600;

struct Upscale {
  // The colour target of the frame's latest depth-tested colour draw, the
  // depth bound with it, and the textures it was resolved into since.
  GuestTexture *scene_surface = nullptr;
  GuestTexture *scene_depth = nullptr;
  std::vector<GuestTexture *> scene_textures;
  bool upscaled_this_frame = false;

  // The upscaled scene, and the scene textures it stands in for until one of
  // them is resolved again.
  std::unique_ptr<GuestTexture> output;
  std::vector<const GuestTexture *> output_for;

  // The composite's target at the output scale, and the game's target it
  // replaces until the frame ends.
  std::unique_ptr<GuestTexture> composite;
  GuestTexture *composite_source = nullptr;

  bool camera_captured = false;
  Matrix view_proj{};
  bool previous_valid = false;
  Matrix previous_view_proj{};
  u32 camera_mismatches = 0;

  u32 jitter_index = 0;
  f32 jitter_x = 0.0f, jitter_y = 0.0f;
  u64 frame = 0;
  u64 last_upscaled_frame = 0;
};

Upscale &upscale() {
  static Upscale u;
  return u;
}

bool Enabled() { return UpscalerActive() && metalfx::Available(); }

bool IsColorSurface(const GuestTexture *t) {
  return t && t->texture && t->type == ResourceType::RenderTarget && t->followsSceneScale &&
         t->guestHeight == kGameHeight;
}

// The full-size 1280x720 target the composite and HUD draw into.
bool IsCompositeSurface(const GuestTexture *t) {
  return IsColorSurface(t) && t->guestWidth == kGameWidth;
}

void AdvanceJitter(Upscale &u) {
  u.jitter_index = (u.jitter_index % JitterPhases(SceneRenderScale() / SceneInputScale())) + 1;
  u.jitter_x = Halton(u.jitter_index, 2) - 0.5f;
  u.jitter_y = Halton(u.jitter_index, 3) - 0.5f;
}

Matrix ReadViewProjection(u32 device_guest) {
  const auto *device = mem::at<const D3DDevice>(device_guest);
  Matrix m{};
  for (u32 row = 0; row < 4; ++row)
    for (u32 col = 0; col < 4; ++col)
      m[row * 4 + col] = f32(device->vsFloatConstants[kViewProjectionRegister + row][col]);
  return m;
}

// Parks a texture this module owns until the frame using it retires.
void RetireOwnedLocked(VideoState &s, std::unique_ptr<GuestTexture> &t) {
  if (!t)
    return;
  // A retired output no longer stands in for the scene textures.
  if (t == upscale().output)
    upscale().output_for.clear();
  RetireTextureBindingsLocked(s, t.get());
  for (GuestTexture *dst : t->destinationTextures)
    dst->sourceSurface = nullptr;
  t->destinationTextures.clear();
  if (s.render_target == t.get())
    s.render_target = nullptr;
  if (s.last_drawn_rt == t.get())
    s.last_drawn_rt = nullptr;
  const u32 slot = Video::RetireSlot("upscaler texture");
  if (t->textureView)
    s.texture_view_graveyard[slot].push_back(std::move(t->textureView));
  if (t->textureHolder)
    s.texture_graveyard[slot].push_back(std::move(t->textureHolder));
  t.reset();
}

// Keeps 't' a 'width' x 'height' texture of 'format', rebuilding it (as a
// render target, plus a shader-writable one when 'storage') when that changes.
GuestTexture *EnsureOwnedLocked(VideoState &s, std::unique_ptr<GuestTexture> &t, u32 width,
                                u32 height, plume::RenderFormat format, bool storage,
                                const char *tag) {
  if (t && t->width == width && t->height == height && t->format == format)
    return t.get();
  RetireOwnedLocked(s, t);
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width;
  desc.height = height;
  desc.depth = 1;
  desc.mipLevels = 1;
  desc.arraySize = 1;
  desc.format = format;
  desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  if (storage)
    desc.flags |= plume::RenderTextureFlag::STORAGE;
  auto texture = std::make_unique<GuestTexture>(storage ? ResourceType::Texture
                                                        : ResourceType::RenderTarget);
  texture->textureHolder = CreateHostTexture(s.device.get(), desc, tag);
  texture->texture = texture->textureHolder.get();
  if (!texture->texture)
    return nullptr;
  texture->viewDimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  texture->format = format;
  texture->width = width;
  texture->height = height;
  if (BindTextureSRVLocked(s, texture.get()) == kInvalidDescriptorIndex)
    return nullptr;
  t = std::move(texture);
  return t.get();
}

void Transition(VideoState &s, GuestTexture *t, plume::RenderTextureLayout layout) {
  if (t->layout == layout)
    return;
  plume::RenderTextureBarrier barrier(t->texture, layout);
  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, &barrier, 1);
  t->layout = layout;
}

metalfx::Frame BuildFrame(const Upscale &u, GuestTexture *color, GuestTexture *depth,
                          GuestTexture *output) {
  metalfx::Frame frame;
  frame.color = color->texture;
  frame.depth = depth->texture;
  frame.output = output->texture;
  frame.input_width = color->width;
  frame.input_height = color->height;
  frame.jitter_x = u.jitter_x;
  frame.jitter_y = u.jitter_y;
  Matrix reproject = kIdentity;
  frame.reset = !u.camera_captured || !u.previous_valid ||
                u.last_upscaled_frame + 1 != u.frame ||
                !CameraReprojection(u.view_proj, u.previous_view_proj, kCameraCutNdc, reproject);
  if (frame.reset)
    reproject = kIdentity;
  std::copy(reproject.begin(), reproject.end(), frame.reproject);
  return frame;
}

void LogFrame(const Upscale &u, const metalfx::Frame &frame, const GuestTexture *output) {
  if (!DevMode() || u.frame % kDevLogInterval != 0)
    return;
  MGRN_DEV_WARN("[upscale] frame {}: {}x{} -> {}x{} jitter ({:.3f},{:.3f}) reset {} "
                "scene textures {} camera mismatches {}",
                u.frame, frame.input_width, frame.input_height, output->width, output->height,
                u.jitter_x, u.jitter_y, frame.reset, u.scene_textures.size(),
                u.camera_mismatches);
}

// Splits the frame around the MetalFX pass. False leaves the scene textures
// to be sampled as they are (the input-size scene, stretched).
bool UpscaleSceneLocked(VideoState &s, Upscale &u) {
  GuestTexture *color = u.scene_surface;
  GuestTexture *depth = u.scene_depth;
  if (!depth || !depth->texture || color->sampleCount != plume::RenderSampleCount::COUNT_1)
    return false;
  GuestTexture *output = EnsureOwnedLocked(
      s, u.output, ScaleExtent(color->guestWidth, SceneRenderScale()),
      ScaleExtent(color->guestHeight, SceneRenderScale()), color->format, true, "upscaled-scene");
  if (!output)
    return false;
  Transition(s, color, plume::RenderTextureLayout::SHADER_READ);
  Transition(s, depth, plume::RenderTextureLayout::SHADER_READ);
  Transition(s, output, plume::RenderTextureLayout::GENERAL);
  if (!SubmitSplitLocked(s))
    return false;
  const metalfx::Frame frame = BuildFrame(u, color, depth, output);
  const bool encoded = metalfx::Encode(frame);
  BeginCommandList(s);
  if (!encoded || !s.command_list_open)
    return false;
  Transition(s, output, plume::RenderTextureLayout::SHADER_READ);
  LogFrame(u, frame, output);
  return true;
}

// Points the composite at an output-size copy of its target for the rest of
// the frame.
void RedirectCompositeLocked(VideoState &s, Upscale &u, GuestTexture *target) {
  GuestTexture *composite =
      EnsureOwnedLocked(s, u.composite, ScaleExtent(target->guestWidth, SceneRenderScale()),
                        ScaleExtent(target->guestHeight, SceneRenderScale()), target->format,
                        false, "upscaled-composite");
  if (!composite)
    return;
  composite->followsSceneScale = true;
  composite->guestWidth = target->guestWidth;
  composite->guestHeight = target->guestHeight;
  composite->renderScale = SceneRenderScale();
  composite->targetGeneration = RenderTargetGeneration();
  u.composite_source = target;
  s.render_target = composite;
  s.draw_framebuffer_bound = false;
  s.dirtyStates.viewport = true;
  s.dirtyStates.scissorRect = true;
}

void StartCompositeLocked(VideoState &s, Upscale &u, GuestTexture *target) {
  u.upscaled_this_frame = true;
  // Last frame's output stands in for nothing once this frame's scene exists.
  u.output_for.clear();
  // Without an upscaled scene the frame finishes at the input size.
  if (!UpscaleSceneLocked(s, u) || !s.command_list_open)
    return;
  u.output_for.assign(u.scene_textures.begin(), u.scene_textures.end());
  u.last_upscaled_frame = u.frame;
  RedirectCompositeLocked(s, u, target);
}

} // namespace

void InitDevice(VideoState &s) {
  if (metalfx::Init(s.device.get(), s.queue.get()))
    SetMaxUpscaleRatio(metalfx::MaxScale());
}

void BeforeDrawLocked(VideoState &s, u32 device_guest) {
  auto &u = upscale();
  GuestTexture *rt = s.render_target;
  if (!Enabled() || u.composite_source || !IsColorSurface(rt) || !device_guest)
    return;
  const auto *device = mem::at<const D3DDevice>(device_guest);
  const bool z_enable = ReadDeviceRegister(*device, kRegDepthControl) & kDepthControlZEnable;
  const bool color_write = ReadDeviceRegister(*device, kRegColorMask, kColorMaskAll) != 0;
  if (z_enable && color_write) {
    u.scene_surface = rt;
    u.scene_depth = s.depth_stencil && s.depth_stencil->followsSceneScale ? s.depth_stencil
                                                                            : nullptr;
    u.scene_textures.clear();
  } else if (!z_enable && !u.upscaled_this_frame && !u.scene_textures.empty() &&
             IsCompositeSurface(rt)) {
    StartCompositeLocked(s, u, rt);
  }
}

void NoteResolveLocked(VideoState &s, GuestTexture *src, GuestTexture *dst) {
  auto &u = upscale();
  if (std::find(u.output_for.begin(), u.output_for.end(), dst) != u.output_for.end())
    u.output_for.clear();
  if (Enabled() && src && src == u.scene_surface && dst && !IsDepthFormat(dst->format))
    u.scene_textures.push_back(dst);
}

void SceneDrawOffsetLocked(VideoState &s, u32 device_guest, f32 &x, f32 &y) {
  x = y = 0.0f;
  auto &u = upscale();
  GuestTexture *rt = s.render_target;
  if (!Enabled() || u.composite_source || !IsColorSurface(rt) || !s.pipelineState.zEnable ||
      !device_guest)
    return;
  if (s.pipelineState.zWriteEnable) {
    const Matrix view_proj = ReadViewProjection(device_guest);
    if (!u.camera_captured) {
      u.view_proj = view_proj;
      u.camera_captured = true;
    } else if (view_proj != u.view_proj) {
      ++u.camera_mismatches;
    }
  }
  const f32 width = s.viewport.width * rt->renderScale;
  const f32 height = s.viewport.height * rt->renderScale;
  if (width <= 0.0f || height <= 0.0f)
    return;
  x = 2.0f * u.jitter_x / width;
  y = -2.0f * u.jitter_y / height;
}

f32 SceneMipBiasLocked(const VideoState &s) {
  if (!Enabled() || upscale().composite_source || !IsColorSurface(s.render_target))
    return 0.0f;
  return std::log2(SceneInputScale() / SceneRenderScale());
}

GuestTexture *SubstituteLocked(const GuestTexture *texture) {
  auto &u = upscale();
  if (!texture || !u.output || u.output->descriptorIndex == kInvalidDescriptorIndex ||
      std::find(u.output_for.begin(), u.output_for.end(), texture) == u.output_for.end())
    return nullptr;
  return u.output.get();
}

GuestTexture *RedirectTarget(GuestTexture *surface) {
  auto &u = upscale();
  return surface && surface == u.composite_source && u.composite ? u.composite.get() : surface;
}

void ForgetTextureLocked(VideoState &s, GuestTexture *dead) {
  auto &u = upscale();
  std::erase(u.output_for, dead);
  std::erase(u.scene_textures, dead);
  if (u.scene_surface == dead || u.scene_depth == dead) {
    u.scene_surface = nullptr;
    u.scene_depth = nullptr;
    u.scene_textures.clear();
  }
  if (u.composite_source == dead) {
    if (u.composite && s.render_target == u.composite.get())
      s.render_target = nullptr;
    u.composite_source = nullptr;
  }
}

void EndFrameLocked(VideoState &s) {
  auto &u = upscale();
  if (u.composite_source && u.composite && s.render_target == u.composite.get()) {
    s.render_target = u.composite_source;
    s.draw_framebuffer_bound = false;
  }
  u.composite_source = nullptr;
  if (!Enabled()) {
    RetireOwnedLocked(s, u.output);
    RetireOwnedLocked(s, u.composite);
    u.output_for.clear();
    u.previous_valid = false;
  } else if (u.upscaled_this_frame && u.camera_captured) {
    u.previous_view_proj = u.view_proj;
    u.previous_valid = true;
  }
  u.scene_surface = u.scene_depth = nullptr;
  u.scene_textures.clear();
  u.upscaled_this_frame = false;
  u.camera_captured = false;
  ++u.frame;
  AdvanceJitter(u);
}

} // namespace mgrn::gpu::upscale
