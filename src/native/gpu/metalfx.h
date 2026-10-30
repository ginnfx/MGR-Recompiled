/**
 * @file    gpu/metalfx.h
 * @brief   MetalFX temporal upscaling of the scene, run on the Metal objects
 *          MoltenVK exports (VK_EXT_metal_objects) between two Vulkan
 *          submissions of one frame.
 */
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderCommandQueue;
struct RenderDevice;
struct RenderTexture;
} // namespace plume

namespace mgrn::gpu::metalfx {

// Exports the Metal device and the queue's Metal command queue, and builds the
// motion vector kernel. False (with the reason logged) when the device lacks
// VK_EXT_metal_objects or MetalFX temporal scaling.
bool Init(plume::RenderDevice *device, plume::RenderCommandQueue *queue);
void Shutdown();
bool Available();
// The largest output/input ratio per axis the device's temporal scaler takes.
f32 MaxScale();

struct Frame {
  // Scene colour and depth at the input size; colour's top-left
  // input_width x input_height is the scene, and depth covers at least that.
  plume::RenderTexture *color = nullptr;
  plume::RenderTexture *depth = nullptr;
  // Written in full; its size is the output size.
  plume::RenderTexture *output = nullptr;
  u32 input_width = 0;
  u32 input_height = 0;
  // Sub-pixel offset the scene's geometry was shifted by, in input pixels
  // (+x right, +y down).
  f32 jitter_x = 0.0f;
  f32 jitter_y = 0.0f;
  // Row-major: takes this frame's unjittered NDC position (x, y, z, 1) to the
  // previous frame's clip position. Camera motion only.
  f32 reproject[16] = {};
  // Depth stores 1 at the near plane (MGR's reversed viewport).
  bool depth_reversed = true;
  // Drop the history (camera cut, first frame, size change).
  bool reset = false;
};

// Commits one Metal command buffer that writes motion vectors from depth and
// the upscaled image. The caller has submitted every Vulkan command that
// writes the inputs, and submits the commands that read the output after it,
// all on the exported queue. False if the frame could not be encoded.
bool Encode(const Frame &frame);

} // namespace mgrn::gpu::metalfx
