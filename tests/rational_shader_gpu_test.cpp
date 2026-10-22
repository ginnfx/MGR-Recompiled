// GPU check of the resolution-scaled resolve and texture load shaders.
//
// For each case, a resolve copy shader reads a random EDRAM buffer (addressed
// with the 80x16-sample tile layout at the case's scale) and writes the scaled
// resolve buffer of a tiled texture, and the texture load shader of the same
// scale reads that buffer back into a linear image, which must hold exactly
// the EDRAM pixels of the rectangle (round trip, all scales).
//
// For 3/2, every host pixel must also land at the CPU reference address
// (rex/graphics/util/scaled_texture_address.h) and no other byte may change.
// The vendored integer-scale SPIR-V (ReXGlue, from a newer Xenia than the
// xesl sources here) orders bytes inside a scaled group differently, so for
// integer scales only the round trip and the written byte count are checked.
//
// Runs the production SPIR-V on MoltenVK (Homebrew libMoltenVK.dylib) without
// a window; exits 77 (skipped) when no Vulkan device is available.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <rex/graphics/util/draw.h>
#include <rex/graphics/util/scaled_texture_address.h>
#include <rex/graphics/xenos.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace shaders {
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_8bpp_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_16bpp_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_32bpp_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/resolve_full_128bpp_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_8bpb_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_8bpb_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_16bpb_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_16bpb_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_32bpb_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_32bpb_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_64bpb_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_64bpb_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_128bpb_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_128bpb_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_r5g6b5_rgba8_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_r5g6b5_rgba8_scaled_rational_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_r4g4b4a4_rgba8_scaled_cs.h"
#include "../vendor/rexglue-sdk/src/graphics/shaders/vulkan_spirv/texture_load_r4g4b4a4_rgba8_scaled_rational_cs.h"
}  // namespace shaders

using rex::graphics::GetScaledTexelAddressing;
using rex::graphics::ScaleRatio;
namespace draw_util = rex::graphics::draw_util;
namespace xenos = rex::graphics::xenos;

namespace {

constexpr int kSkipped = 77;
int failures = 0;
int case_failures = 0;  // the first few of each case are printed

#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      ++failures;                                                 \
      if (++case_failures <= 4) {                                 \
        std::fprintf(stderr, "  FAIL line %d: ", __LINE__);       \
        std::fprintf(stderr, __VA_ARGS__);                        \
        std::fprintf(stderr, "\n");                               \
      }                                                           \
    }                                                             \
  } while (0)

// ---------------------------------------------------------------------------
// Minimal headless Vulkan compute context.
struct Vk {
  void* library = nullptr;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties{};
#define VK_FN(name) PFN_##name name = nullptr;
#define VK_FNS(X)                                                                       \
  X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties)              \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceQueue)           \
  X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory)                 \
  X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateShaderModule)                           \
  X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateComputePipelines)   \
  X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)        \
  X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)             \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants)                  \
  X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkEndCommandBuffer) X(vkQueueSubmit)        \
  X(vkQueueWaitIdle) X(vkDestroyPipeline) X(vkDestroyShaderModule)                       \
  X(vkDestroyPipelineLayout) X(vkDestroyDescriptorSetLayout) X(vkDestroyDescriptorPool)  \
  X(vkFreeCommandBuffers) X(vkDestroyBuffer) X(vkFreeMemory)
  VK_FNS(VK_FN)
#undef VK_FN

  // kUnavailable: no MoltenVK or no compute device (skip). kFailed: a device
  // exists but setup failed, which is a broken driver, not a skip.
  enum class Init { kOk, kUnavailable, kFailed };
  Init Initialize();
};

Vk::Init Vk::Initialize() {
  for (const char* path : {"/opt/homebrew/lib/libMoltenVK.dylib", "libMoltenVK.dylib"}) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library) break;
  }
  if (!library) return Init::kUnavailable;
  gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
  if (!gipa) return Init::kFailed;
  auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  if (!create_instance || create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS)
    return Init::kFailed;
#define VK_LOAD(name) name = reinterpret_cast<PFN_##name>(gipa(instance, #name));
  VK_FNS(VK_LOAD)
#undef VK_LOAD
  uint32_t count = 1;
  if (vkEnumeratePhysicalDevices(instance, &count, &physical) < 0 || !count)
    return Init::kUnavailable;
  vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
  for (queue_family = 0; queue_family < family_count; ++queue_family)
    if (families[queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
  if (queue_family == family_count) return Init::kUnavailable;
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) return Init::kFailed;
  vkGetDeviceQueue(device, queue_family, 0, &queue);
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = queue_family;
  return vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS ? Init::kOk
                                                                         : Init::kFailed;
}

// Host-visible coherent storage buffer.
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  uint64_t size = 0;
};

Buffer CreateBuffer(Vk& vk, uint64_t size) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  vk.vkCreateBuffer(vk.device, &info, nullptr, &b.buffer);
  VkMemoryRequirements requirements;
  vk.vkGetBufferMemoryRequirements(vk.device, b.buffer, &requirements);
  const VkMemoryPropertyFlags wanted =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  for (uint32_t i = 0; i < vk.memory_properties.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (vk.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
      allocate.memoryTypeIndex = i;
      break;
    }
  }
  vk.vkAllocateMemory(vk.device, &allocate, nullptr, &b.memory);
  vk.vkBindBufferMemory(vk.device, b.buffer, b.memory, 0);
  vk.vkMapMemory(vk.device, b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.data));
  return b;
}

void DestroyBuffer(Vk& vk, Buffer& b) {
  vk.vkDestroyBuffer(vk.device, b.buffer, nullptr);
  vk.vkFreeMemory(vk.device, b.memory, nullptr);
  b = {};
}

// Runs `code` with set 0 = first buffer, set 1 = second buffer (each binding
// 0), as both the resolve copy and the texture load shaders are laid out.
void Dispatch(Vk& vk, const uint32_t* code, size_t code_size, const Buffer& set0,
              const Buffer& set1, const void* push, uint32_t push_size, uint32_t groups_x,
              uint32_t groups_y, uint32_t groups_z = 1) {
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = code_size;
  module_info.pCode = code;
  VkShaderModule module;
  vk.vkCreateShaderModule(vk.device, &module_info, nullptr, &module);
  VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  VkDescriptorSetLayout set_layout;
  vk.vkCreateDescriptorSetLayout(vk.device, &set_layout_info, nullptr, &set_layout);
  VkDescriptorSetLayout layouts[2] = {set_layout, set_layout};
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &range;
  VkPipelineLayout layout;
  vk.vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &layout);
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
  pipeline_info.layout = layout;
  VkPipeline pipeline;
  vk.vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorPool pool;
  vk.vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &pool);
  VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 2;
  set_info.pSetLayouts = layouts;
  VkDescriptorSet sets[2];
  vk.vkAllocateDescriptorSets(vk.device, &set_info, sets);
  VkDescriptorBufferInfo buffer_infos[2] = {{set0.buffer, 0, VK_WHOLE_SIZE},
                                            {set1.buffer, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet writes[2];
  for (int i = 0; i < 2; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = sets[i];
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vk.vkUpdateDescriptorSets(vk.device, 2, writes, 0, nullptr);
  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = vk.pool;
  command_info.commandBufferCount = 1;
  VkCommandBuffer command;
  vk.vkAllocateCommandBuffers(vk.device, &command_info, &command);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vk.vkBeginCommandBuffer(command, &begin);
  vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0,
                             nullptr);
  vk.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
  vk.vkCmdDispatch(command, groups_x, groups_y, groups_z);
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
  vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  vk.vkEndCommandBuffer(command);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  vk.vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE);
  vk.vkQueueWaitIdle(vk.queue);
  vk.vkFreeCommandBuffers(vk.device, vk.pool, 1, &command);
  vk.vkDestroyDescriptorPool(vk.device, pool, nullptr);
  vk.vkDestroyPipeline(vk.device, pipeline, nullptr);
  vk.vkDestroyPipelineLayout(vk.device, layout, nullptr);
  vk.vkDestroyDescriptorSetLayout(vk.device, set_layout, nullptr);
  vk.vkDestroyShaderModule(vk.device, module, nullptr);
}

// ---------------------------------------------------------------------------
// CPU references.

// Xenos 2D tiled texture address (texture_address.xesli
// XenosTextureTiledAddress2D), in bytes.
uint32_t TiledAddress2D(uint32_t x, uint32_t y, uint32_t pitch_macro_tiles, uint32_t bpb_log2) {
  const uint32_t outer = ((y >> 5) * pitch_macro_tiles + (x >> 5)) << 6;
  const uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
  const uint32_t outer_inner = (outer | inner) << bpb_log2;
  const uint32_t bank = (y >> 4) & 1;
  const uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (outer_inner & 0xF) |
         (((outer_inner >> 4) & 1) << 5) | (((outer_inner >> 5) & 7) << 8) |
         (outer_inner >> 8 << 12);
}

// Byte address of a host pixel in the scaled resolve memory of a tiled 2D
// texture: the CPU reference this test checks the shaders against.
uint32_t ScaledResolveAddress(uint32_t x, uint32_t y, ScaleRatio scale,
                              uint32_t pitch_macro_tiles, uint32_t bpb_log2) {
  const uint32_t position[2] = {x, y};
  const auto addressing = GetScaledTexelAddressing(position, scale, scale, bpb_log2);
  const uint32_t guest = TiledAddress2D(addressing.guest_group_origin[0],
                                        addressing.guest_group_origin[1], pitch_macro_tiles,
                                        bpb_log2);
  return uint32_t(rex::graphics::ScaleArea(scale, scale, guest)) +
         addressing.host_byte_offset_in_guest_group;
}

// EDRAM byte address of a host sample (edram.xesli XeEdramOffsetBytes, no
// depth swap): 80x16-sample tiles at 32bpp, 40x16 at 64bpp, scaled.
uint32_t EdramAddress(uint32_t sample_x, uint32_t sample_y, uint32_t base_tiles,
                      uint32_t pitch_tiles, uint32_t format_ints_log2, ScaleRatio scale) {
  const uint32_t tile_w32 = scale.ScaleExact(80), tile_h = scale.ScaleExact(16);
  const uint32_t tile_w = tile_w32 >> format_ints_log2;
  const uint32_t tile_x = sample_x / tile_w, tile_y = sample_y / tile_h;
  const uint32_t tile = (base_tiles + tile_y * pitch_tiles + tile_x) % 2048;
  const uint32_t in_x = sample_x - tile_x * tile_w, in_y = sample_y - tile_y * tile_h;
  return (tile * tile_w32 * tile_h + ((in_y * tile_w + in_x) << format_ints_log2)) * 4;
}

// ---------------------------------------------------------------------------
// One resolve + texture load case.
struct Case {
  const char* name;
  const uint32_t* resolve_integer;
  size_t resolve_integer_size;
  const uint32_t* resolve_rational;
  size_t resolve_rational_size;
  uint32_t resolve_group_x_log2;  // pixels per group along X
  bool full;                      // format-converting (full) shader
  xenos::MsaaSamples msaa;
  xenos::ColorRenderTargetFormat edram_format;
  xenos::ColorFormat dest_format;
  uint32_t dest_bpb_log2;
  const uint32_t* load_integer;
  size_t load_integer_size;
  const uint32_t* load_rational;
  size_t load_rational_size;
  uint32_t load_blocks_per_thread_log2;
};

#define SPV(name) shaders::name##_cs, sizeof(shaders::name##_cs)
#define SPV2(name) SPV(name##_scaled), SPV(name##_scaled_rational)
const Case kCases[] = {
    {"fast 32bpp 1x", SPV2(resolve_fast_32bpp_1x2xmsaa), 6, false, xenos::MsaaSamples::k1X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8_8_8_8, 2,
     SPV2(texture_load_32bpb), 3},
    {"fast 32bpp 2x", SPV2(resolve_fast_32bpp_1x2xmsaa), 6, false, xenos::MsaaSamples::k2X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8_8_8_8, 2,
     SPV2(texture_load_32bpb), 3},
    {"fast 32bpp 4x", SPV2(resolve_fast_32bpp_4xmsaa), 6, false, xenos::MsaaSamples::k4X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8_8_8_8, 2,
     SPV2(texture_load_32bpb), 3},
    {"fast 64bpp 1x", SPV2(resolve_fast_64bpp_1x2xmsaa), 5, false, xenos::MsaaSamples::k1X,
     xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT, xenos::ColorFormat::k_16_16_16_16_FLOAT,
     3, SPV2(texture_load_64bpb), 2},
    {"fast 64bpp 4x", SPV2(resolve_fast_64bpp_4xmsaa), 5, false, xenos::MsaaSamples::k4X,
     xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT, xenos::ColorFormat::k_16_16_16_16_FLOAT,
     3, SPV2(texture_load_64bpb), 2},
    {"full 8bpp 4x", SPV2(resolve_full_8bpp), 6, true, xenos::MsaaSamples::k4X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8, 0,
     SPV2(texture_load_8bpb), 4},
    {"full 16bpp 1x", SPV2(resolve_full_16bpp), 5, true, xenos::MsaaSamples::k1X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8_8, 1,
     SPV2(texture_load_16bpb), 4},
    {"full 32bpp 2x", SPV2(resolve_full_32bpp), 5, true, xenos::MsaaSamples::k2X,
     xenos::ColorRenderTargetFormat::k_8_8_8_8, xenos::ColorFormat::k_8_8_8_8, 2,
     SPV2(texture_load_32bpb), 3},
    {"full 128bpp 4x", SPV2(resolve_full_128bpp), 4, true, xenos::MsaaSamples::k4X,
     xenos::ColorRenderTargetFormat::k_32_32_FLOAT, xenos::ColorFormat::k_32_32_32_32_FLOAT, 4,
     SPV2(texture_load_128bpb), 1},
};

// Guest rectangle and placement, all multiples of 8 guest pixels. Odd counts
// of 8-pixel units make 3/2 widths and heights 12 * odd host pixels.
struct Placement {
  uint32_t edram_offset_x_div_8, edram_offset_y_div_8, width_div_8, height_div_8;
  uint32_t base_tiles, pitch_tiles;
  uint32_t dest_offset_x_div_8, dest_offset_y_div_8, dest_pitch_div_32, dest_height_div_32;
};
const Placement kPlacements[] = {
    {0, 0, 5, 3, 0, 4, 0, 0, 2, 1},
    {7, 1, 13, 5, 37, 6, 3, 1, 4, 3},
    {9, 0, 11, 9, 100, 5, 1, 2, 4, 4},
};

// Expected texel bits of resolved host pixel (x, y) of the rectangle, as
// written to the destination (fast: raw copy of sample 0).
void ExpectedTexel(const Case& c, const Placement& p, ScaleRatio scale, const Buffer& edram,
                   uint32_t x, uint32_t y, uint32_t out[4]) {
  const uint32_t ints_log2 =
      xenos::IsColorRenderTargetFormat64bpp(c.edram_format) ? 1u : 0u;
  const uint32_t px = x + scale.ScaleExact(p.edram_offset_x_div_8 * 8);
  const uint32_t py = y + scale.ScaleExact(p.edram_offset_y_div_8 * 8);
  const uint32_t sx = px << uint32_t(c.msaa >= xenos::MsaaSamples::k4X);
  const uint32_t sy = py << uint32_t(c.msaa >= xenos::MsaaSamples::k2X);
  const uint32_t pitch_tiles_scaled_format = p.pitch_tiles;
  const uint32_t address =
      EdramAddress(sx, sy, p.base_tiles, pitch_tiles_scaled_format, ints_log2, scale);
  uint32_t words[2];
  std::memcpy(words, edram.data + address, 4u << ints_log2);
  std::memset(out, 0, 16);
  switch (c.dest_bpb_log2) {
    case 0: out[0] = words[0] & 0xFF; break;                    // R8 of RGBA8
    case 1: out[0] = words[0] & 0xFFFF; break;                  // RG8 of RGBA8
    case 2: out[0] = words[0]; break;
    case 3: out[0] = words[0]; out[1] = words[1]; break;
    case 4: out[0] = words[0]; out[1] = words[1]; break;  // RG of 32_32_FLOAT
  }
}

// Random EDRAM contents that every format converts losslessly: RGBA8 bytes,
// normal half floats, and normal float32 values.
void FillEdram(Buffer& edram, xenos::ColorRenderTargetFormat format, uint32_t seed) {
  std::mt19937 rng(seed);
  uint32_t* words = reinterpret_cast<uint32_t*>(edram.data);
  const uint64_t count = edram.size / 4;
  for (uint64_t i = 0; i < count; ++i) {
    uint32_t v = rng();
    if (format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT) {
      // Exponent 1..29 in both halves keeps them normal and finite.
      auto normal_half = [](uint32_t h) { return (h & 0x83FF) | ((1 + (h >> 10) % 29) << 10); };
      v = normal_half(v & 0xFFFF) | (normal_half(v >> 16) << 16);
    } else if (format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
      v = (v & 0x807FFFFF) | ((64 + (v >> 23) % 128) << 23);
    }
    words[i] = v;
  }
}

// Loads the whole scaled texture into a linear image of `host_bpb`-byte
// texels, rows padded to whole threads; returns the row pitch in bytes.
uint32_t LoadScaledTexture(Vk& vk, const uint32_t* code, size_t code_size, const Buffer& source,
                           Buffer& image, ScaleRatio scale, uint32_t guest_width,
                           uint32_t guest_height, uint32_t host_bpb,
                           uint32_t blocks_per_thread_log2) {
  const uint32_t host_width = scale.ScaleCeil(guest_width);
  const uint32_t host_height = scale.ScaleCeil(guest_height);
  const uint32_t blocks_per_thread = 1u << blocks_per_thread_log2;
  const uint32_t pitch =
      (host_width + blocks_per_thread - 1) / blocks_per_thread * blocks_per_thread * host_bpb;
  image = CreateBuffer(vk, uint64_t(pitch) * host_height + 256);
  const uint32_t push[10] = {
      1u | (scale.num << 4) | (scale.num << 7),  // tiled, 2D, endian none
      0, guest_width, guest_height,              // guest offset, pitch, z stride
      host_width, host_height, 1,                // size_blocks
      0, pitch, guest_height};                   // host offset and pitch
  const uint32_t threads_x = (host_width + blocks_per_thread - 1) >> blocks_per_thread_log2;
  Dispatch(vk, code, code_size, image, source, push, sizeof(push), (threads_x + 3) / 4,
           (host_height + 31) / 32);
  return pitch;
}

uint32_t BuildDestInfo(xenos::ColorFormat format) {
  return uint32_t(format) << 7;  // endian none, 2D, slice 0, no bias, no swap
}

void RunCase(Vk& vk, const Case& c, const Placement& p, ScaleRatio scale, bool fill) {
  const bool rational = !scale.IsInteger();
  const std::string label = std::string(c.name) + " at " + std::to_string(scale.num) + "/" +
                            std::to_string(scale.den) + (fill ? " (half-pixel fill)" : "");
  const uint32_t ints_log2 = xenos::IsColorRenderTargetFormat64bpp(c.edram_format) ? 1u : 0u;
  Buffer edram = CreateBuffer(vk, rex::graphics::ScaleArea(scale, scale, xenos::kEdramSizeBytes));
  FillEdram(edram, c.edram_format, 1234 + uint32_t(c.dest_bpb_log2) * 7 + p.base_tiles);

  // Destination: the scaled resolve memory of the whole guest texture.
  const uint32_t bpb = 1u << c.dest_bpb_log2;
  const uint32_t guest_width = p.dest_pitch_div_32 * 32, guest_height = p.dest_height_div_32 * 32;
  const uint64_t guest_bytes = uint64_t(guest_width) * guest_height * bpb;
  Buffer dest = CreateBuffer(vk, rex::graphics::ScaleArea(scale, scale, guest_bytes));
  std::memset(dest.data, 0xA5, dest.size);

  draw_util::ResolveEdramInfo edram_info;
  edram_info.pitch_tiles = p.pitch_tiles;
  edram_info.msaa_samples = c.msaa;
  edram_info.base_tiles = p.base_tiles;
  edram_info.format = uint32_t(c.edram_format);
  edram_info.format_is_64bpp = ints_log2;
  edram_info.fill_half_pixel_offset = fill;
  draw_util::ResolveCoordinateInfo coordinate_info;
  coordinate_info.edram_offset_x_div_8 = p.edram_offset_x_div_8;
  coordinate_info.edram_offset_y_div_8 = p.edram_offset_y_div_8;
  coordinate_info.width_div_8 = p.width_div_8;
  coordinate_info.draw_resolution_scale_x = scale.num;
  coordinate_info.draw_resolution_scale_y = scale.num;
  draw_util::ResolveCopyDestCoordinateInfo dest_coordinate_info;
  dest_coordinate_info.pitch_aligned_div_32 = p.dest_pitch_div_32;
  dest_coordinate_info.height_aligned_div_32 = p.dest_height_div_32;
  dest_coordinate_info.offset_x_div_8 = p.dest_offset_x_div_8;
  dest_coordinate_info.offset_y_div_8 = p.dest_offset_y_div_8;
  dest_coordinate_info.copy_sample_select = xenos::CopySampleSelect::k0;
  const uint32_t push[4] = {edram_info.packed, coordinate_info.packed,
                            BuildDestInfo(c.dest_format), dest_coordinate_info.packed};
  const uint32_t width = scale.ScaleExact(p.width_div_8 * 8);
  const uint32_t height = scale.ScaleExact(p.height_div_8 * 8);
  const uint32_t group_y_log2 = rational ? draw_util::kResolveCopyRationalGroupSizeYLog2 : 3;
  Dispatch(vk, rational ? c.resolve_rational : c.resolve_integer,
           rational ? c.resolve_rational_size : c.resolve_integer_size, edram, dest, push,
           sizeof(push), (width + (1u << c.resolve_group_x_log2) - 1) >> c.resolve_group_x_log2,
           height >> group_y_log2);

  // 3/2: every rectangle pixel at its reference address, everything else
  // intact. Integer: as many bytes written as the rectangle holds.
  const uint32_t dest_x0 = scale.ScaleExact(p.dest_offset_x_div_8 * 8);
  const uint32_t dest_y0 = scale.ScaleExact(p.dest_offset_y_div_8 * 8);
  const uint32_t fill_rows = fill ? (scale.num + scale.den - 1) / (2 * scale.den) : 0;
  case_failures = 0;
  if (rational) {
    std::vector<uint8_t> written(dest.size, 0);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t address = ScaledResolveAddress(dest_x0 + x, dest_y0 + y, scale,
                                                      p.dest_pitch_div_32, c.dest_bpb_log2);
        uint32_t expected[4];
        ExpectedTexel(c, p, scale, edram, std::max(x, fill_rows), std::max(y, fill_rows),
                      expected);
        uint32_t actual[4] = {};
        std::memcpy(actual, dest.data + address, bpb);
        if (c.dest_bpb_log2 == 4) actual[2] = actual[3] = 0;  // only RG come from 32_32
        CHECK(std::memcmp(actual, expected, bpb) == 0,
              "%s: pixel (%u, %u) at %u: %08X %08X, expected %08X %08X", label.c_str(), x, y,
              address, actual[0], actual[1], expected[0], expected[1]);
        for (uint32_t b = 0; b < bpb; ++b) written[address + b] = 1;
      }
    }
    for (uint64_t i = 0; i < dest.size; ++i)
      CHECK(written[i] || dest.data[i] == 0xA5, "%s: byte %llu outside the rectangle changed",
            label.c_str(), (unsigned long long)i);
  } else {
    uint64_t changed = 0;
    for (uint64_t i = 0; i < dest.size; ++i) changed += dest.data[i] != 0xA5;
    CHECK(changed <= uint64_t(width) * height * bpb, "%s: %llu bytes written for %u pixels",
          label.c_str(), (unsigned long long)changed, width * height);
  }

  // Texture load of the scaled texture back to a linear image.
  Buffer image;
  const uint32_t host_pitch_bytes = LoadScaledTexture(
      vk, rational ? c.load_rational : c.load_integer,
      rational ? c.load_rational_size : c.load_integer_size, dest, image, scale, guest_width,
      guest_height, bpb, c.load_blocks_per_thread_log2);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t expected[4];
      ExpectedTexel(c, p, scale, edram, std::max(x, fill_rows), std::max(y, fill_rows), expected);
      uint32_t actual[4] = {};
      std::memcpy(actual, image.data + (dest_y0 + y) * host_pitch_bytes + (dest_x0 + x) * bpb,
                  bpb);
      if (c.dest_bpb_log2 == 4) actual[2] = actual[3] = 0;
      CHECK(std::memcmp(actual, expected, bpb) == 0,
            "%s: loaded texel (%u, %u): %08X %08X, expected %08X %08X", label.c_str(), x, y,
            actual[0], actual[1], expected[0], expected[1]);
    }
  }
  std::printf("%-44s %s\n", label.c_str(), case_failures ? "FAILED" : "ok");
  DestroyBuffer(vk, image);
  DestroyBuffer(vk, dest);
  DestroyBuffer(vk, edram);
}

// R5G6B5 / R4G4B4A4 to RGBA8 (ReXGlue's hand-written SPIR-V; alpha of 5_6_5
// repeats blue there).
uint32_t ExpandR5G6B5(uint32_t t) {
  const uint32_t r = t & 31, g = (t >> 5) & 63, b = t >> 11;
  const uint32_t b8 = (b << 3) | (b >> 2);
  return ((r << 3) | (r >> 2)) | (((g << 2) | (g >> 4)) << 8) | (b8 << 16) | (b8 << 24);
}
uint32_t ExpandR4G4B4A4(uint32_t t) {
  const uint32_t n = (t & 0xF) | ((t & 0xF0) << 4) | ((t & 0xF00) << 8) | ((t & 0xF000) << 12);
  return n | (n << 4);
}

// The 16bpb-to-RGBA8 loads must expand the 16-bit texel at the CPU reference
// address. At 3/2 these are the xesl rebuilds, also checked against the plain
// 16bpb load. At integer scales they are ReXGlue's hand-written SPIR-V, which
// follows the upstream Xenia integer layout of the CPU reference, while the
// other vendored integer shaders lay groups out differently: resolved R5G6B5
// and R4G4B4A4 textures read wrong texels at 2x and 3x (pre-existing ReXGlue
// issue, outside the 3/2 work), so the integer plain 16bpb load is not compared.
void RunRgba8ExpansionCase(Vk& vk, ScaleRatio scale) {
  const bool rational = !scale.IsInteger();
  case_failures = 0;
  const uint32_t guest_width = 128, guest_height = 64;
  Buffer source = CreateBuffer(vk, rex::graphics::ScaleArea(scale, scale,
                                                            uint64_t(guest_width) * guest_height * 2));
  std::mt19937 rng(77);
  for (uint64_t i = 0; i < source.size / 4; ++i) reinterpret_cast<uint32_t*>(source.data)[i] = rng();
  struct Code {
    const uint32_t* code;
    size_t size;
  };
#define CODE(name) Code{shaders::name##_cs, sizeof(shaders::name##_cs)}
  const Code load_raw = rational ? CODE(texture_load_16bpb_scaled_rational)
                                 : CODE(texture_load_16bpb_scaled);
  const Code load_565 = rational ? CODE(texture_load_r5g6b5_rgba8_scaled_rational)
                                 : CODE(texture_load_r5g6b5_rgba8_scaled);
  const Code load_4444 = rational ? CODE(texture_load_r4g4b4a4_rgba8_scaled_rational)
                                  : CODE(texture_load_r4g4b4a4_rgba8_scaled);
#undef CODE
  Buffer raw, r565, r4444;
  const uint32_t raw_pitch = LoadScaledTexture(vk, load_raw.code, load_raw.size, source, raw,
                                               scale, guest_width, guest_height, 2, 4);
  const uint32_t pitch = LoadScaledTexture(vk, load_565.code, load_565.size, source, r565, scale,
                                           guest_width, guest_height, 4, 4);
  LoadScaledTexture(vk, load_4444.code, load_4444.size, source, r4444, scale, guest_width,
                    guest_height, 4, 4);
  const std::string label = "16bpb to RGBA8 at " + std::to_string(scale.num) + "/" +
                            std::to_string(scale.den);
  for (uint32_t y = 0; y < scale.ScaleCeil(guest_height); ++y) {
    for (uint32_t x = 0; x < scale.ScaleCeil(guest_width); ++x) {
      uint16_t texel;
      std::memcpy(&texel, source.data + ScaledResolveAddress(x, y, scale, guest_width / 32, 1), 2);
      if (rational) {
        uint16_t loaded;
        std::memcpy(&loaded, raw.data + y * raw_pitch + x * 2, 2);
        CHECK(loaded == texel, "%s: 16bpb (%u, %u) %04X, expected %04X", label.c_str(), x, y,
              loaded, texel);
      }
      uint32_t a, b;
      std::memcpy(&a, r565.data + y * pitch + x * 4, 4);
      std::memcpy(&b, r4444.data + y * pitch + x * 4, 4);
      CHECK(a == ExpandR5G6B5(texel), "%s: 565 (%u, %u) %08X", label.c_str(), x, y, a);
      CHECK(b == ExpandR4G4B4A4(texel), "%s: 4444 (%u, %u) %08X", label.c_str(), x, y, b);
    }
  }
  std::printf("%-44s %s\n", label.c_str(), case_failures ? "FAILED" : "ok");
  DestroyBuffer(vk, r4444);
  DestroyBuffer(vk, r565);
  DestroyBuffer(vk, raw);
  DestroyBuffer(vk, source);
}

}  // namespace

int main() {
  Vk vk;
  switch (vk.Initialize()) {
    case Vk::Init::kOk:
      break;
    case Vk::Init::kUnavailable:
      std::printf("no Vulkan device (MoltenVK); skipped\n");
      return kSkipped;
    case Vk::Init::kFailed:
      std::printf("FAIL: Vulkan device setup failed\n");
      return 1;
  }
  const ScaleRatio scales[] = {ScaleRatio::Integer(2), ScaleRatio::Integer(3), ScaleRatio{3, 2}};
  for (const Case& c : kCases) {
    for (const ScaleRatio scale : scales) {
      for (const Placement& p : kPlacements) RunCase(vk, c, p, scale, false);
      RunCase(vk, c, kPlacements[1], scale, true);
    }
  }
  for (const ScaleRatio scale : scales) RunRgba8ExpansionCase(vk, scale);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("resolution-scaled resolve and texture load shaders match the CPU reference\n");
  return 0;
}
