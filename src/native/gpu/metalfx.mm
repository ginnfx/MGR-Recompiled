/**
 * @file    gpu/metalfx.mm
 * @brief   MetalFX temporal upscaling on MoltenVK's Metal objects (compiled
 *          with ARC).
 *
 * MoltenVK runs each VkQueue on one MTLCommandQueue and commits a submission's
 * Metal command buffer before vkQueueSubmit returns (synchronous submits, its
 * default). A command buffer committed here between two submissions therefore
 * runs between them, and Metal's hazard tracking (MoltenVK textures are
 * tracked) orders the texture reads and writes across the three.
 */
#include "gpu/metalfx.h"

// Before the Apple frameworks: Security.framework defines an INTEL macro that
// breaks plume's vendor enum.
#include <plume_vulkan.h>

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <simd/simd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "core/logging.h"

namespace mgrn::gpu::metalfx {

namespace {

// camera_motion: motion vectors from depth, each pixel's unjittered position
// this frame reprojected into the previous frame, in input pixels (current to
// previous, MetalFX's convention).
// restore_alpha: MetalFX rebuilds colour only, but MGR's composite reads the
// scene's alpha (its glare weight), so the output takes the input's alpha.
constexpr const char *kKernels = R"(
#include <metal_stdlib>
using namespace metal;

struct MotionParams {
  float4x4 reproject;
  float2 size;
  float2 jitter_ndc;
  float2 jitter_px;
  uint depth_reversed;
};

kernel void camera_motion(depth2d<float, access::read> depth [[texture(0)]],
                          texture2d<float, access::write> motion [[texture(1)]],
                          constant MotionParams &p [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= uint(p.size.x) || gid.y >= uint(p.size.y))
    return;
  const float d = depth.read(gid);
  const float z = p.depth_reversed != 0 ? 1.0 - d : d;
  const float2 pix = float2(gid) + 0.5;
  const float2 ndc = float2(pix.x / p.size.x * 2.0 - 1.0, 1.0 - pix.y / p.size.y * 2.0) -
                     p.jitter_ndc;
  const float4 prev = p.reproject * float4(ndc, z, 1.0);
  float2 mv = float2(0.0);
  if (prev.w > 1e-6) {
    const float2 prev_ndc = prev.xy / prev.w;
    const float2 prev_pix = float2((prev_ndc.x + 1.0) * 0.5 * p.size.x,
                                   (1.0 - prev_ndc.y) * 0.5 * p.size.y);
    mv = prev_pix - (pix - p.jitter_px);
  }
  motion.write(float4(mv, 0.0, 0.0), gid);
}

kernel void restore_alpha(texture2d<float, access::sample> color [[texture(0)]],
                          texture2d<float, access::read_write> output [[texture(1)]],
                          constant float2 &input_size [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
  const float2 output_size = float2(output.get_width(), output.get_height());
  if (gid.x >= uint(output_size.x) || gid.y >= uint(output_size.y))
    return;
  constexpr sampler linear(filter::linear, address::clamp_to_edge, coord::normalized);
  const float2 uv = (float2(gid) + 0.5) / output_size * input_size /
                    float2(color.get_width(), color.get_height());
  float4 value = output.read(gid);
  value.a = color.sample(linear, uv).a;
  output.write(value, gid);
}
)";

struct MotionParams {
  simd_float4x4 reproject;
  simd_float2 size;
  simd_float2 jitter_ndc;
  simd_float2 jitter_px;
  uint32_t depth_reversed;
};

constexpr MTLPixelFormat kMotionFormat = MTLPixelFormatRG16Float;
constexpr NSUInteger kMotionGroupSize = 16;
// Fallback when the device does not report its range (pre-macOS 14 API).
constexpr float kDefaultMaxScale = 2.0f;
constexpr u32 kMaxLoggedErrors = 8;

struct ScalerKey {
  MTLPixelFormat color = MTLPixelFormatInvalid, depth = MTLPixelFormatInvalid,
                 output = MTLPixelFormatInvalid;
  NSUInteger in_w = 0, in_h = 0, out_w = 0, out_h = 0;
  bool operator==(const ScalerKey &) const = default;
};

struct State {
  VkDevice vk_device = VK_NULL_HANDLE;
  PFN_vkExportMetalObjectsEXT export_objects = nullptr;
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLComputePipelineState> motion_pipeline = nil;
  id<MTLComputePipelineState> alpha_pipeline = nil;
  id<MTLTexture> motion = nil;
  id<MTLFXTemporalScaler> scaler = nil;
  ScalerKey scaler_key{};
  float max_scale = 1.0f;
  bool scaler_failed = false;
};

State &state() {
  static State s;
  return s;
}

std::atomic<u32> g_logged_errors{0};

bool ShouldLogError() {
  return g_logged_errors.fetch_add(1, std::memory_order_relaxed) < kMaxLoggedErrors;
}

id<MTLTexture> ExportTexture(State &s, plume::RenderTexture *texture) {
  auto *vk_texture = static_cast<plume::VulkanTexture *>(texture);
  if (!vk_texture || vk_texture->vk == VK_NULL_HANDLE)
    return nil;
  VkExportMetalTextureInfoEXT texture_info{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
  texture_info.image = vk_texture->vk;
  texture_info.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
  VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
  info.pNext = &texture_info;
  s.export_objects(s.vk_device, &info);
  return texture_info.mtlTexture;
}

id<MTLComputePipelineState> BuildPipeline(State &s, id<MTLLibrary> library, NSString *name) {
  NSError *error = nil;
  id<MTLFunction> function = [library newFunctionWithName:name];
  id<MTLComputePipelineState> pipeline =
      function ? [s.device newComputePipelineStateWithFunction:function error:&error] : nil;
  if (!pipeline)
    MGRN_ERROR("[metalfx] kernel {} failed to build: {}", name.UTF8String,
               error ? error.localizedDescription.UTF8String : "no function");
  return pipeline;
}

bool BuildPipelines(State &s) {
  NSError *error = nil;
  id<MTLLibrary> library = [s.device newLibraryWithSource:@(kKernels)
                                                  options:[MTLCompileOptions new]
                                                    error:&error];
  if (!library) {
    MGRN_ERROR("[metalfx] kernels failed to compile: {}",
               error ? error.localizedDescription.UTF8String : "unknown");
    return false;
  }
  s.motion_pipeline = BuildPipeline(s, library, @"camera_motion");
  s.alpha_pipeline = BuildPipeline(s, library, @"restore_alpha");
  return s.motion_pipeline && s.alpha_pipeline;
}

bool EnsureMotionTexture(State &s, u32 width, u32 height) {
  if (s.motion && s.motion.width == width && s.motion.height == height)
    return true;
  MTLTextureDescriptor *desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kMotionFormat
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  desc.storageMode = MTLStorageModePrivate;
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  s.motion = [s.device newTextureWithDescriptor:desc];
  return s.motion != nil;
}

// Rebuilds the scaler when a size or format changes. A new scaler starts
// without history, so the caller resets.
bool EnsureScaler(State &s, const ScalerKey &key, bool &rebuilt) {
  rebuilt = false;
  if (s.scaler && s.scaler_key == key)
    return true;
  if (s.scaler_failed && s.scaler_key == key)
    return false;
  s.scaler = nil;
  s.scaler_key = key;
  MTLFXTemporalScalerDescriptor *desc = [MTLFXTemporalScalerDescriptor new];
  desc.colorTextureFormat = key.color;
  desc.depthTextureFormat = key.depth;
  desc.motionTextureFormat = kMotionFormat;
  desc.outputTextureFormat = key.output;
  desc.inputWidth = key.in_w;
  desc.inputHeight = key.in_h;
  desc.outputWidth = key.out_w;
  desc.outputHeight = key.out_h;
  desc.autoExposureEnabled = NO;
  s.scaler = [desc newTemporalScalerWithDevice:s.device];
  s.scaler_failed = s.scaler == nil;
  if (s.scaler_failed) {
    MGRN_ERROR("[metalfx] temporal scaler {}x{} -> {}x{} (colour {} depth {} output {}) "
               "was not created",
               key.in_w, key.in_h, key.out_w, key.out_h, u32(key.color), u32(key.depth),
               u32(key.output));
    return false;
  }
  MGRN_INFO("[metalfx] temporal scaler {}x{} -> {}x{} (colour {} depth {} output {})", key.in_w,
            key.in_h, key.out_w, key.out_h, u32(key.color), u32(key.depth), u32(key.output));
  rebuilt = true;
  return true;
}

bool HasUsage(id<MTLTexture> texture, MTLTextureUsage needed, const char *what) {
  if ((texture.usage & needed) == needed)
    return true;
  if (ShouldLogError())
    MGRN_ERROR("[metalfx] {} texture usage 0x{:X} lacks 0x{:X}", what, u64(texture.usage),
               u64(needed));
  return false;
}

simd_float4x4 ColumnMajor(const f32 (&rows)[16]) {
  simd_float4x4 m;
  for (int c = 0; c < 4; ++c)
    m.columns[c] = simd_make_float4(rows[c], rows[4 + c], rows[8 + c], rows[12 + c]);
  return m;
}

void EncodeMotion(State &s, id<MTLCommandBuffer> cb, id<MTLTexture> depth, const Frame &f) {
  MotionParams params;
  params.reproject = ColumnMajor(f.reproject);
  params.size = simd_make_float2(f.input_width, f.input_height);
  params.jitter_ndc =
      simd_make_float2(2.0f * f.jitter_x / f.input_width, -2.0f * f.jitter_y / f.input_height);
  params.jitter_px = simd_make_float2(f.jitter_x, f.jitter_y);
  params.depth_reversed = f.depth_reversed ? 1u : 0u;
  id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
  [encoder setComputePipelineState:s.motion_pipeline];
  [encoder setTexture:depth atIndex:0];
  [encoder setTexture:s.motion atIndex:1];
  [encoder setBytes:&params length:sizeof(params) atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(f.input_width, f.input_height, 1)
      threadsPerThreadgroup:MTLSizeMake(kMotionGroupSize, kMotionGroupSize, 1)];
  [encoder endEncoding];
}

void EncodeAlpha(State &s, id<MTLCommandBuffer> cb, id<MTLTexture> color, id<MTLTexture> output,
                 const Frame &f) {
  const simd_float2 input_size = simd_make_float2(f.input_width, f.input_height);
  id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
  [encoder setComputePipelineState:s.alpha_pipeline];
  [encoder setTexture:color atIndex:0];
  [encoder setTexture:output atIndex:1];
  [encoder setBytes:&input_size length:sizeof(input_size) atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(output.width, output.height, 1)
      threadsPerThreadgroup:MTLSizeMake(kMotionGroupSize, kMotionGroupSize, 1)];
  [encoder endEncoding];
}

} // namespace

bool Init(plume::RenderDevice *device, plume::RenderCommandQueue *queue) {
  auto &s = state();
  if (s.device)
    return true;
  auto *vk_device = static_cast<plume::VulkanDevice *>(device);
  auto *vk_queue = static_cast<plume::VulkanCommandQueue *>(queue);
  if (!vk_device || !vk_queue || !vk_queue->queue)
    return false;
  // The MetalFX pass is ordered between two Vulkan submissions only when
  // MoltenVK commits each one inside vkQueueSubmit (native_window.cpp sets
  // this unless the environment overrides it).
  const char *synchronous = std::getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS");
  if (synchronous && std::strcmp(synchronous, "0") == 0) {
    MGRN_INFO("[metalfx] unavailable: needs MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1");
    return false;
  }
  s.vk_device = vk_device->vk;
  s.export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
      vkGetDeviceProcAddr(s.vk_device, "vkExportMetalObjectsEXT"));
  if (!s.export_objects) {
    MGRN_INFO("[metalfx] unavailable: the Vulkan device has no VK_EXT_metal_objects");
    return false;
  }
  @autoreleasepool {
    VkExportMetalCommandQueueInfoEXT queue_info{
        VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
    queue_info.queue = vk_queue->queue->vk;
    VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    device_info.pNext = &queue_info;
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
    info.pNext = &device_info;
    s.export_objects(s.vk_device, &info);
    s.device = device_info.mtlDevice;
    s.queue = queue_info.mtlCommandQueue;
    if (!s.device || !s.queue) {
      MGRN_INFO("[metalfx] unavailable: MoltenVK exported no Metal device or queue");
      Shutdown();
      return false;
    }
    if (![MTLFXTemporalScalerDescriptor supportsDevice:s.device]) {
      MGRN_INFO("[metalfx] unavailable: {} has no temporal scaling", s.device.name.UTF8String);
      Shutdown();
      return false;
    }
    if (@available(macOS 14.0, *))
      s.max_scale = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:s.device];
    if (!(s.max_scale > 1.0f))
      s.max_scale = kDefaultMaxScale;
    // restore_alpha reads and writes the RGBA8 output in place.
    if (s.device.readWriteTextureSupport < MTLReadWriteTextureTier2) {
      MGRN_INFO("[metalfx] unavailable: {} cannot read and write RGBA8 textures",
                s.device.name.UTF8String);
      Shutdown();
      return false;
    }
    if (!BuildPipelines(s)) {
      Shutdown();
      return false;
    }
  }
  MGRN_INFO("[metalfx] temporal upscaling available, up to {:.2f}x per axis", s.max_scale);
  return true;
}

void Shutdown() {
  auto &s = state();
  s = State{};
}

bool Available() { return state().motion_pipeline != nil; }

f32 MaxScale() { return Available() ? state().max_scale : 1.0f; }

bool Encode(const Frame &f) {
  auto &s = state();
  if (!Available() || !f.color || !f.depth || !f.output || !f.input_width || !f.input_height)
    return false;
  @autoreleasepool {
    id<MTLTexture> color = ExportTexture(s, f.color);
    id<MTLTexture> depth = ExportTexture(s, f.depth);
    id<MTLTexture> output = ExportTexture(s, f.output);
    if (!color || !depth || !output) {
      if (ShouldLogError())
        MGRN_ERROR("[metalfx] texture export failed (colour {} depth {} output {})",
                   color != nil, depth != nil, output != nil);
      return false;
    }
    if (color.width < f.input_width || color.height < f.input_height ||
        depth.width < f.input_width || depth.height < f.input_height) {
      if (ShouldLogError())
        MGRN_ERROR("[metalfx] inputs {}x{} / {}x{} smaller than the scene {}x{}", color.width,
                   color.height, depth.width, depth.height, f.input_width, f.input_height);
      return false;
    }
    const ScalerKey key{color.pixelFormat, depth.pixelFormat, output.pixelFormat,
                        f.input_width,     f.input_height,    output.width,
                        output.height};
    bool rebuilt = false;
    if (!EnsureScaler(s, key, rebuilt) || !EnsureMotionTexture(s, f.input_width, f.input_height))
      return false;
    if (!HasUsage(color, s.scaler.colorTextureUsage, "colour") ||
        !HasUsage(depth, s.scaler.depthTextureUsage, "depth") ||
        !HasUsage(output, s.scaler.outputTextureUsage, "output"))
      return false;

    id<MTLCommandBuffer> cb = [s.queue commandBuffer];
    cb.label = @"MGR MetalFX";
    EncodeMotion(s, cb, depth, f);

    s.scaler.colorTexture = color;
    s.scaler.depthTexture = depth;
    s.scaler.motionTexture = s.motion;
    s.scaler.outputTexture = output;
    s.scaler.inputContentWidth = f.input_width;
    s.scaler.inputContentHeight = f.input_height;
    // The scaler takes the sample position within the pixel, the opposite of
    // the shift applied to the geometry (of the four sign combinations, this
    // one keeps the most detail on a still scene).
    s.scaler.jitterOffsetX = -f.jitter_x;
    s.scaler.jitterOffsetY = -f.jitter_y;
    s.scaler.motionVectorScaleX = 1.0f;
    s.scaler.motionVectorScaleY = 1.0f;
    s.scaler.depthReversed = f.depth_reversed;
    s.scaler.reset = f.reset || rebuilt;
    [s.scaler encodeToCommandBuffer:cb];
    EncodeAlpha(s, cb, color, output, f);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      if (done.status == MTLCommandBufferStatusError && ShouldLogError())
        MGRN_ERROR("[metalfx] command buffer failed: {}",
                   done.error ? done.error.localizedDescription.UTF8String : "unknown");
    }];
    [cb commit];
  }
  return true;
}

} // namespace mgrn::gpu::metalfx
