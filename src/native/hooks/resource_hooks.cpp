// Native renderer: guest D3D object creation and host resource lookup.
// Texture creation adapted from re:Blue gpu/hooks/resource.cpp (BSD-3,
// Copyright (c) 2026 Tom Clay).

#include "hooks/resources.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/format.h"
#include "gpu/guest_write_watch.h"
#include "gpu/host_resource_heap.h"
#include "gpu/native_texture_mirror.h"
#include "gpu/resources.h"
#include "gpu/settings.h"
#include "gpu/surface_registry.h"
#include "gpu/vertex_declaration.h"
#include "hooks/hook_macros.h"
#include "hooks/texture_fetch.h"

namespace mgrn::native {

namespace {

namespace gpu = mgrn::gpu;
using mgrn::NativeRendererActive;

// D3DRESOURCETYPE values D3DDevice_CreateTexture takes.
constexpr u32 kD3DRTypeTexture = 3;
constexpr u32 kD3DRTypeVolumeTexture = 17;
constexpr u32 kD3DRTypeCubeTexture = 18;

// Arguments of a guest D3DDevice_CreateTexture, kept until the texture is first
// used as a resolve destination (most are; the rest are sampled from guest
// memory through the native mirror).
struct TextureCreateArgs {
  u32 width, height, depth, levels, usage, format, type;
};

// Arguments of a guest D3DDevice_CreateSurface, for surfaces created before
// the host device existed.
struct SurfaceCreateArgs {
  u32 width, height, format, multisample;
};

std::mutex g_mutex;
std::unordered_map<u32, TextureCreateArgs> g_created_textures;
std::unordered_map<u32, SurfaceCreateArgs> g_created_surfaces;
// Fetch constant each native mirror was built from: a different one means the
// engine reused the D3DTexture for other texels, so the mirror is rebuilt.
std::unordered_map<u32, gpu::GpuTextureFetchConstant> g_mirror_fetch;
// Fetch constants the mirror could not build, so a rejected texture is not
// rebuilt (and logged) on every bind.
std::unordered_map<u32, gpu::GpuTextureFetchConstant> g_rejected_fetch;
// write_watch stamp each mirror's texels were read under, so a mirror of
// memory the guest rewrites (movie frames) is rebuilt.
std::unordered_map<u32, gpu::write_watch::Stamp> g_mirror_stamp;
// Guest memory a resolve writes, keyed by mip 0's physical address, naming the
// destination texture's VA. The engine samples resolved memory through other
// D3DTexture headers too (views it builds over the same allocation), so those
// find the host texture by address rather than by header.
std::unordered_map<u32, u32> g_resolved_memory;

TextureCreateArgs ArgsFromFetch(const TextureFetchDesc& desc) {
  namespace xe = rex::graphics::xenos;
  const u32 type = desc.dimension == xe::DataDimension::k3D     ? kD3DRTypeVolumeTexture
                   : desc.dimension == xe::DataDimension::kCube ? kD3DRTypeCubeTexture
                                                                : kD3DRTypeTexture;
  return {desc.width, desc.height, desc.depth, desc.levels, 0, desc.d3d_format, type};
}

bool SameFetch(const std::unordered_map<u32, gpu::GpuTextureFetchConstant>& map, u32 va,
               const gpu::GpuTextureFetchConstant& fetch) {
  auto it = map.find(va);
  return it != map.end() && std::memcmp(&it->second, &fetch, sizeof(fetch)) == 0;
}

void RememberResolvedMemory(u32 texture_va) {
  TextureFetchDesc desc;
  if (!DecodeTextureFetch(texture_va, desc) || !desc.base_address) return;
  std::lock_guard lock(g_mutex);
  g_resolved_memory[desc.base_address] = texture_va;
}

// The resolve destination whose memory the texture at 'texture_va' samples,
// when its fetch matches that destination's size and format.
gpu::GuestTexture* FindResolvedAlias(u32 texture_va) {
  TextureFetchDesc desc;
  if (!DecodeTextureFetch(texture_va, desc) || !desc.base_address) return nullptr;
  u32 owner_va = 0;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_resolved_memory.find(desc.base_address);
    if (it == g_resolved_memory.end()) return nullptr;
    owner_va = it->second;
  }
  auto* owner = gpu::HostResourceHeap::FromGuest<gpu::GuestTexture>(owner_va);
  if (!owner || !owner->texture) return nullptr;
  // Host dimensions carry the render scale; compare in host texels.
  const bool matches = owner->width == gpu::ScaleExtent(desc.width, owner->renderScale) &&
                       owner->height == gpu::ScaleExtent(desc.height, owner->renderScale) &&
                       owner->format == gpu::ConvertGuestFormat(desc.d3d_format);
  if (!matches) {
    static std::atomic<u32> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 16)
      MGRN_WARN("native: texture 0x{:08X} ({}) views resolve target 0x{:08X} ({}x{} format {}) "
                "with a different layout",
                texture_va, DescribeFetch(desc), owner_va, owner->width, owner->height,
                u32(owner->format));
    return nullptr;
  }
  return owner;
}

// Scene-shaped targets record their guest size and the scale they were built
// at, so a later mgr_resolution_scale change can resize them in place.
void MarkSceneScale(gpu::GuestTexture* texture, u32 guest_width, u32 guest_height,
                    bool eligible) {
  texture->followsSceneScale = eligible && gpu::IsSceneShapedTarget(guest_width, guest_height);
  texture->guestWidth = guest_width;
  texture->guestHeight = guest_height;
  texture->sceneScaleGeneration = gpu::SceneScaleGeneration();
  texture->renderScale = texture->followsSceneScale ? gpu::SceneRenderScale()
                         : eligible && gpu::IsShadowShapedTarget(guest_width, guest_height)
                             ? gpu::ShadowRenderScale()
                             : 1.0f;
}

// Scene targets follow mgr_resolution_scale; shadow targets mgr_shadow_resolution.
f32 SceneScaleFor(u32 guest_width, u32 guest_height, bool eligible) {
  if (!eligible) return 1.0f;
  if (gpu::IsSceneShapedTarget(guest_width, guest_height)) return gpu::SceneRenderScale();
  if (gpu::IsShadowShapedTarget(guest_width, guest_height)) return gpu::ShadowRenderScale();
  return 1.0f;
}

// Rebuilds a scene-shaped target at the current scale when the setting has
// changed since it was built. The guest redraws it every frame.
gpu::GuestTexture* EnsureSceneScale(gpu::GuestTexture* texture) {
  if (!texture || !texture->followsSceneScale ||
      texture->sceneScaleGeneration == gpu::SceneScaleGeneration())
    return texture;
  const f32 scale = gpu::SceneRenderScale();
  gpu::Video::ResizeTexture(texture, gpu::ScaleExtent(texture->guestWidth, scale),
                            gpu::ScaleExtent(texture->guestHeight, scale));
  texture->renderScale = scale;
  texture->sceneScaleGeneration = gpu::SceneScaleGeneration();
  return texture;
}

gpu::GuestTexture* CreateSurfaceTexture(u32 surface_va, const SurfaceCreateArgs& args) {
  // MGR renders without MSAA. The setting multisamples the full-size scene
  // colour and depth surfaces, so the pairs drawn together always match.
  const bool scene_surface = args.width == gpu::kGameWidth && args.height == gpu::kGameHeight;
  const auto msaa = (args.multisample != 0 || scene_surface)
                        ? gpu::Video::CvarMSAASampleCount()
                        : plume::RenderSampleCounts(plume::RenderSampleCount::COUNT_1);
  if (gpu::ConvertGuestFormat(args.format) == plume::RenderFormat::UNKNOWN) return nullptr;
  const f32 scale = SceneScaleFor(args.width, args.height, true);
  auto* surface = gpu::SurfaceRegistry::Get(gpu::ScaleExtent(args.width, scale),
                                            gpu::ScaleExtent(args.height, scale), args.format,
                                            u32(msaa));
  if (!surface) return nullptr;
  MarkSceneScale(surface, args.width, args.height, true);
  // The registry recycles entries whose reference count reads 0, which in
  // re:Blue the guest maintained. Here the guest object is separate, so pin it.
  surface->x360.as_surface.resource.ReferenceCount = 1u;
  gpu::HostResourceHeap::Attach(surface_va, surface);
  return surface;
}

gpu::GuestTexture* CreateRenderableTexture(u32 texture_va, const TextureCreateArgs& args) {
  const bool is_volume = args.type == kD3DRTypeVolumeTexture;
  const bool is_cube = args.type == kD3DRTypeCubeTexture;
  const auto rtype = is_volume ? gpu::ResourceType::VolumeTexture : gpu::ResourceType::Texture;
  const auto view_dimension = is_volume ? plume::RenderTextureViewDimension::TEXTURE_3D
                              : is_cube ? plume::RenderTextureViewDimension::TEXTURE_CUBE
                                        : plume::RenderTextureViewDimension::TEXTURE_2D;
  auto* device = gpu::Video::HostDevice();
  if (!device) return nullptr;
  const plume::RenderFormat format = gpu::ConvertGuestFormat(args.format);
  if (format == plume::RenderFormat::UNKNOWN) return nullptr;  // logged by ConvertGuestFormat

  // Resolve destinations of scaled screen targets are scaled with them.
  const bool scalable = !is_volume && !is_cube;
  const f32 scale = SceneScaleFor(args.width, args.height, scalable);
  auto* texture = gpu::HostResourceHeap::Alloc<gpu::GuestTexture>(rtype);
  plume::RenderTextureDesc desc;
  desc.dimension = is_volume ? plume::RenderTextureDimension::TEXTURE_3D
                             : plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = gpu::ScaleExtent(args.width, scale);
  desc.height = gpu::ScaleExtent(args.height, scale);
  desc.depth = is_volume ? args.depth : 1;
  desc.mipLevels = args.levels ? args.levels : 1;
  desc.arraySize = is_cube ? 6 : 1;
  desc.format = format;
  if (gpu::IsDepthFormat(format)) {
    desc.flags = plume::RenderTextureFlag::DEPTH_TARGET;
  } else if (gpu::IsRenderTargetCapable(format)) {
    desc.flags = plume::RenderTextureFlag::RENDER_TARGET;
  } else {
    desc.flags = plume::RenderTextureFlag::NONE;
  }
  if (is_cube) desc.flags |= plume::RenderTextureFlag::CUBE;
  desc.committed = false;

  texture->viewDimension = view_dimension;
  texture->textureHolder = gpu::CreateHostTexture(device, desc, "guest-texture");
  texture->texture = texture->textureHolder.get();
  if (texture->texture && gpu::IsDepthFormat(format)) {
    // BindTextureSRV creates the backend's depth-aspect sampling view.
    texture->format = format;
    texture->mipLevels = desc.mipLevels;
    gpu::Video::BindTextureSRV(texture);
  } else if (texture->texture) {
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = format;
    view_desc.dimension = view_dimension;
    view_desc.mipLevels = desc.mipLevels;
    texture->textureView = texture->texture->createTextureView(view_desc);
    gpu::Video::BindTextureSRV(texture);
  }
  texture->width = desc.width;
  texture->height = desc.height;
  MarkSceneScale(texture, args.width, args.height, scalable);
  texture->depth = args.depth;
  texture->mipLevels = desc.mipLevels;
  texture->format = format;
  texture->guestFormat = args.format;
  gpu::HostResourceHeap::Attach(texture_va, texture);
  return texture;
}

}  // namespace

gpu::GuestTexture* ResolveSurface(u32 surface_va) {
  if (!surface_va) return nullptr;
  if (auto* host = gpu::HostResourceHeap::FromGuest<gpu::GuestTexture>(surface_va))
    return EnsureSceneScale(host);
  SurfaceCreateArgs args;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_created_surfaces.find(surface_va);
    if (it == g_created_surfaces.end()) {
      MGRN_DEV_WARN("native: surface 0x{:08X} was not created through CreateSurface", surface_va);
      return nullptr;
    }
    args = it->second;
  }
  return CreateSurfaceTexture(surface_va, args);
}

gpu::GuestTexture* ResolveDestination(u32 texture_va) {
  if (!texture_va) return nullptr;
  if (auto* host = gpu::HostResourceHeap::FromGuest<gpu::GuestTexture>(texture_va)) {
    // The engine may have pointed the header at other memory since.
    RememberResolvedMemory(texture_va);
    return EnsureSceneScale(host);
  }
  TextureCreateArgs args;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_created_textures.find(texture_va);
    if (it != g_created_textures.end()) {
      args = it->second;
    } else {
      // Built by the engine without CreateTexture: take the shape from the
      // fetch constant the guest runtime filled in.
      TextureFetchDesc desc;
      if (!DecodeTextureFetch(texture_va, desc)) return nullptr;
      args = ArgsFromFetch(desc);
      MGRN_INFO("native: adopting resolve target 0x{:08X} from its fetch ({})", texture_va,
                DescribeFetch(desc));
    }
  }
  auto* texture = CreateRenderableTexture(texture_va, args);
  if (texture) RememberResolvedMemory(texture_va);
  return texture;
}

gpu::GuestTexture* LookupPresentTexture(u32 texture_va) {
  if (!texture_va) return nullptr;
  if (auto* host = gpu::HostResourceHeap::FromGuest<gpu::GuestTexture>(texture_va)) return host;
  return FindResolvedAlias(texture_va);
}

gpu::GuestTexture* ResolveTexture(u32 texture_va) {
  if (!texture_va) return nullptr;
  if (auto* host = gpu::HostResourceHeap::FromGuest<gpu::GuestTexture>(texture_va))
    return EnsureSceneScale(host);
  if (auto* alias = FindResolvedAlias(texture_va)) return EnsureSceneScale(alias);
  const auto* guest = mgrn::mem::try_at<const gpu::D3DTexture>(texture_va);
  if (!guest) return nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (SameFetch(g_rejected_fetch, texture_va, guest->Format)) return nullptr;
  }
  TextureFetchDesc desc;
  if (!DecodeTextureFetch(texture_va, desc)) return nullptr;
  {
    std::lock_guard lock(g_mutex);
    if (SameFetch(g_mirror_fetch, texture_va, guest->Format) &&
        !gpu::write_watch::WrittenSince(desc.base_address, desc.size_bytes,
                                        g_mirror_stamp[texture_va])) {
      if (auto* mirror = gpu::ResolveGuestTexture(texture_va)) return mirror;
    }
    g_mirror_fetch[texture_va] = guest->Format;
  }
  // Armed before the mirror reads the texels, so no write is missed.
  const auto stamp = gpu::write_watch::Arm(desc.base_address, desc.size_bytes);
  auto* mirror = gpu::GetOrCreateNativeMirror(texture_va);
  std::lock_guard lock(g_mutex);
  if (mirror)
    g_mirror_stamp[texture_va] = stamp;
  else
    g_rejected_fetch[texture_va] = guest->Format;
  return mirror;
}

}  // namespace mgrn::native

namespace {
namespace gpu = mgrn::gpu;
namespace native = mgrn::native;
using mgrn::NativeRendererActive;
}  // namespace

// D3DDevice_CreateSurface(Width, Height, Format, MultiSample, pParameters).
MGRN_EXTERN(D3DDevice_CreateSurface);
MGRN_HOOK_RAW(D3DDevice_CreateSurface) {
  const native::SurfaceCreateArgs args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  MGRN_IMP(D3DDevice_CreateSurface)(ctx, base);
  const u32 surface = ctx.r3.u32;
  if (!NativeRendererActive() || !surface) return;
  std::lock_guard lock(native::g_mutex);
  native::g_created_surfaces[surface] = args;
}

// D3DDevice_CreateTexture(Width, Height, Depth, Levels, Usage, Format, Pool, Type).
MGRN_EXTERN(D3DDevice_CreateTexture);
MGRN_HOOK_RAW(D3DDevice_CreateTexture) {
  const native::TextureCreateArgs args{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                       ctx.r7.u32, ctx.r8.u32, ctx.r10.u32};
  MGRN_IMP(D3DDevice_CreateTexture)(ctx, base);
  const u32 texture = ctx.r3.u32;
  if (!NativeRendererActive() || !texture) return;
  std::lock_guard lock(native::g_mutex);
  native::g_created_textures[texture] = args;
}

// D3DDevice_CreateVertexShader / CreatePixelShader(const DWORD* pFunction).
// The stage comes from the container itself (flags 0x102A1101 vertex,
// 0x102A1100 pixel), so a misnamed entry point cannot mislabel the shader.
namespace {
void AttachShader(u32 function, u32 shader, gpu::ResourceType hooked_as) {
  const auto* container = mgrn::mem::at<const be_u32>(function);
  if (!container) return;
  const bool vertex = (u32(container[0]) & 1u) != 0;
  const auto type = vertex ? gpu::ResourceType::VertexShader : gpu::ResourceType::PixelShader;
  static std::atomic<bool> warned{false};
  if (type != hooked_as && !warned.exchange(true))
    MGRN_WARN("native: shader create hook {} received a {} shader container",
              hooked_as == gpu::ResourceType::VertexShader ? "VS" : "PS", vertex ? "vertex" : "pixel");
  if (auto* host = gpu::CreateShader(container, type)) gpu::HostResourceHeap::Attach(shader, host);
}
}  // namespace

MGRN_EXTERN(D3DDevice_CreateVertexShader);
MGRN_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const u32 function = ctx.r3.u32;
  MGRN_IMP(D3DDevice_CreateVertexShader)(ctx, base);
  if (NativeRendererActive() && ctx.r3.u32 && function)
    AttachShader(function, ctx.r3.u32, gpu::ResourceType::VertexShader);
}

MGRN_EXTERN(D3DDevice_CreatePixelShader);
MGRN_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const u32 function = ctx.r3.u32;
  MGRN_IMP(D3DDevice_CreatePixelShader)(ctx, base);
  if (NativeRendererActive() && ctx.r3.u32 && function)
    AttachShader(function, ctx.r3.u32, gpu::ResourceType::PixelShader);
}

// D3DDevice_CreateVertexDeclaration(const D3DVERTEXELEMENT9* pElements).
MGRN_EXTERN(D3DDevice_CreateVertexDeclaration);
MGRN_HOOK_RAW(D3DDevice_CreateVertexDeclaration) {
  const u32 elements = ctx.r3.u32;
  MGRN_IMP(D3DDevice_CreateVertexDeclaration)(ctx, base);
  const u32 decl = ctx.r3.u32;
  if (!NativeRendererActive() || !decl || !elements) return;
  if (auto* host = gpu::CreateVertexDeclaration(mgrn::mem::at<gpu::GuestVertexElement>(elements)))
    gpu::HostResourceHeap::Attach(decl, host);
}
