// MGR port: native pixel shaders (see include/rex/graphics/vulkan/mgr_native_ps.h).

#include <rex/graphics/vulkan/mgr_native_ps.h>

#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace rex::graphics::vulkan::mgr_native {
namespace {

constexpr uint32_t kBundleMagic = 0x424E5258;  // "XRNB"
constexpr uint32_t kBundleVersion = 1;
constexpr uint32_t kSetsPerPool = 1024;
constexpr size_t kUploadPageSize = 2 * 1024 * 1024;

constexpr uint32_t kSpvOpVariable = 59;
constexpr uint32_t kSpvOpDecorate = 71;
constexpr uint32_t kSpvDecorationLocation = 30;
constexpr uint32_t kSpvStorageClassInput = 1;
constexpr size_t kSpvHeaderWords = 5;

bool IsStatsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("MGR_GPU_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}

// Collects the word indices of Location literals on Input variables.
bool ParseInputLocations(PixelShader& shader) {
  const std::vector<uint32_t>& words = shader.spirv;
  if (words.size() < kSpvHeaderWords || words[0] != 0x07230203) return false;
  std::vector<std::pair<uint32_t, size_t>> location_decorations;  // (target id, literal word)
  std::vector<uint32_t> input_variables;
  for (size_t i = kSpvHeaderWords; i < words.size();) {
    uint32_t count = words[i] >> 16, opcode = words[i] & 0xFFFF;
    if (!count || i + count > words.size()) return false;
    if (opcode == kSpvOpDecorate && count >= 4 && words[i + 2] == kSpvDecorationLocation) {
      location_decorations.emplace_back(words[i + 1], i + 3);
    } else if (opcode == kSpvOpVariable && count >= 4 && words[i + 3] == kSpvStorageClassInput) {
      input_variables.push_back(words[i + 2]);
    }
    i += count;
  }
  for (const auto& [target, literal_word] : location_decorations) {
    if (std::find(input_variables.begin(), input_variables.end(), target) ==
        input_variables.end()) {
      continue;  // fragment outputs (SV_Target) also carry Location
    }
    uint32_t reg = words[literal_word];
    if (reg >= 32) return false;
    shader.input_register_mask |= 1u << reg;
    shader.input_location_words.push_back(uint32_t(literal_word));
  }
  return true;
}

}  // namespace

bool NativePixelShaders::Initialize(const ui::vulkan::VulkanDevice* device) {
  device_ = device;
  const char* toggle = std::getenv("MGR_NATIVE_PS");
  if (toggle && toggle[0] == '0') {
    REXGPU_INFO("MGR native PS: disabled by MGR_NATIVE_PS=0");
    return true;
  }
  const char* path = std::getenv("MGR_NATIVE_SHADERS");
  if (!path || !*path || !LoadBundle(path)) {
    REXGPU_WARN("MGR native PS: no shader bundle (set MGR_NATIVE_SHADERS); using Xenia shaders");
    return true;
  }

  VkDescriptorSetLayoutBinding bindings[kBindingCount] = {};
  const VkDescriptorType types[kBindingCount] = {
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLER};
  for (uint32_t i = 0; i < kBindingCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = types[i];
    bindings[i].descriptorCount = i < kBindingTextures2D ? 1 : kSlotCount;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = kBindingCount;
  layout_info.pBindings = bindings;
  const auto& dfn = device_->functions();
  if (dfn.vkCreateDescriptorSetLayout(device_->device(), &layout_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    REXGPU_ERROR("MGR native PS: descriptor set layout creation failed");
    return false;
  }
  storage_alignment_ = size_t(device_->properties().minStorageBufferOffsetAlignment);
  upload_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kUploadPageSize);
  enabled_ = true;
  REXGPU_INFO("MGR native PS: enabled with {} shaders from {}", shaders_.size(), path);
  return true;
}

bool NativePixelShaders::LoadBundle(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  auto read = [&data](size_t& pos, void* out, size_t n) {
    if (pos + n > data.size()) return false;
    std::memcpy(out, data.data() + pos, n);
    pos += n;
    return true;
  };
  size_t pos = 0;
  uint32_t magic, version, count;
  if (!read(pos, &magic, 4) || !read(pos, &version, 4) || !read(pos, &count, 4) ||
      magic != kBundleMagic || version != kBundleVersion) {
    REXGPU_ERROR("MGR native PS: {} is not a v{} native bundle", path, kBundleVersion);
    return false;
  }
  size_t rejected = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t ucode_hash, container_hash;
    uint32_t stage, spec_mask, size;
    if (!read(pos, &ucode_hash, 8) || !read(pos, &container_hash, 8) || !read(pos, &stage, 4) ||
        !read(pos, &spec_mask, 4) || !read(pos, &size, 4) || pos + size > data.size() ||
        size % 4) {
      REXGPU_ERROR("MGR native PS: truncated bundle {}", path);
      return false;
    }
    if (stage == 1 && !shaders_.count(ucode_hash)) {
      PixelShader shader;
      shader.ucode_hash = ucode_hash;
      shader.spec_constants_mask = spec_mask;
      shader.spirv.resize(size / 4);
      std::memcpy(shader.spirv.data(), data.data() + pos, size);
      if (ParseInputLocations(shader)) {
        shaders_.emplace(ucode_hash, std::move(shader));
      } else {
        ++rejected;
      }
    }
    pos += size;
  }
  if (rejected) REXGPU_WARN("MGR native PS: rejected {} malformed shaders", rejected);
  return !shaders_.empty();
}

void NativePixelShaders::Shutdown() {
  if (!device_) return;
  const auto& dfn = device_->functions();
  VkDevice device = device_->device();
  for (auto& [key, module] : modules_) dfn.vkDestroyShaderModule(device, module, nullptr);
  modules_.clear();
  auto destroy_pool = [&](VkDescriptorPool pool) { dfn.vkDestroyDescriptorPool(device, pool, nullptr); };
  for (auto& frame : used_pools_) std::for_each(frame.pools.begin(), frame.pools.end(), destroy_pool);
  std::for_each(free_pools_.begin(), free_pools_.end(), destroy_pool);
  used_pools_.clear();
  free_pools_.clear();
  current_pool_ = VK_NULL_HANDLE;
  upload_pool_.reset();
  if (set_layout_) dfn.vkDestroyDescriptorSetLayout(device, set_layout_, nullptr);
  set_layout_ = VK_NULL_HANDLE;
  enabled_ = false;
}

const PixelShader* NativePixelShaders::Find(uint64_t ucode_hash) const {
  auto it = shaders_.find(ucode_hash);
  return it != shaders_.end() ? &it->second : nullptr;
}

VkShaderModule NativePixelShaders::GetModule(const PixelShader& shader, uint32_t interpolator_mask) {
  if (shader.input_register_mask & ~interpolator_mask) return VK_NULL_HANDLE;
  uint64_t key = shader.ucode_hash ^ (uint64_t(interpolator_mask) * 0x9E3779B97F4A7C15ull);
  auto it = modules_.find(key);
  if (it != modules_.end()) return it->second;
  std::vector<uint32_t> spirv = shader.spirv;
  for (uint32_t word : shader.input_location_words) {
    uint32_t reg = spirv[word];
    spirv[word] = uint32_t(std::popcount(interpolator_mask & ((1u << reg) - 1)));
  }
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(device_, spirv.data(),
                                                               spirv.size() * sizeof(uint32_t));
  modules_.emplace(key, module);  // cache failures (null) too
  return module;
}

VkDescriptorPool NativePixelShaders::CreatePool() {
  if (!free_pools_.empty()) {
    VkDescriptorPool pool = free_pools_.back();
    free_pools_.pop_back();
    return pool;
  }
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSetsPerPool},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kSlotCount * kSetsPerPool},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kSlotCount * kSetsPerPool}};
  VkDescriptorPoolCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = kSetsPerPool;
  info.poolSizeCount = uint32_t(std::size(sizes));
  info.pPoolSizes = sizes;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (device_->functions().vkCreateDescriptorPool(device_->device(), &info, nullptr, &pool) !=
      VK_SUCCESS) {
    REXGPU_ERROR("MGR native PS: descriptor pool creation failed");
    return VK_NULL_HANDLE;
  }
  return pool;
}

VkDescriptorSet NativePixelShaders::AllocateSet(uint64_t frame) {
  if (used_pools_.empty() || used_pools_.back().frame != frame) {
    used_pools_.push_back({frame, {}});
    current_pool_ = VK_NULL_HANDLE;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (current_pool_ == VK_NULL_HANDLE) {
      current_pool_ = CreatePool();
      if (current_pool_ == VK_NULL_HANDLE) return VK_NULL_HANDLE;
      used_pools_.back().pools.push_back(current_pool_);
    }
    VkDescriptorSetAllocateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = current_pool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (device_->functions().vkAllocateDescriptorSets(device_->device(), &info, &set) ==
        VK_SUCCESS) {
      return set;
    }
    current_pool_ = VK_NULL_HANDLE;  // exhausted: open another pool for this frame
  }
  return VK_NULL_HANDLE;
}

void NativePixelShaders::Reclaim(uint64_t completed_frame) {
  if (!enabled_) return;
  auto it = used_pools_.begin();
  for (; it != used_pools_.end() && it->frame <= completed_frame; ++it) {
    for (VkDescriptorPool pool : it->pools) {
      device_->functions().vkResetDescriptorPool(device_->device(), pool, 0);
      free_pools_.push_back(pool);
    }
  }
  if (it != used_pools_.begin()) {
    used_pools_.erase(used_pools_.begin(), it);
    if (used_pools_.empty()) current_pool_ = VK_NULL_HANDLE;
  }
  upload_pool_->Reclaim(completed_frame);
}

void NativePixelShaders::FlushWrites() {
  if (enabled_) upload_pool_->FlushWrites();
}

uint8_t* NativePixelShaders::RequestUpload(uint64_t frame, size_t size, VkBuffer& buffer,
                                           VkDeviceSize& offset) {
  return upload_pool_->Request(frame, size, storage_alignment_, buffer, offset);
}

void NativePixelShaders::CountDraw(bool native, const char* fallback_reason) {
  if (!IsStatsEnabled()) return;
  (native ? native_draws_ : fallback_draws_)++;
  if (!native) fallback_reasons_[fallback_reason]++;
  constexpr uint64_t kReportEvery = 20000;
  if (native_draws_ + fallback_draws_ < kReportEvery) return;
  std::string reasons;
  for (const auto& [reason, n] : fallback_reasons_) reasons += " " + reason + "=" + std::to_string(n);
  std::fprintf(stderr, "[mgr-native] draws native %llu, fallback %llu (%.1f%% native);%s\n",
               (unsigned long long)native_draws_, (unsigned long long)fallback_draws_,
               100.0 * double(native_draws_) / double(native_draws_ + fallback_draws_),
               reasons.c_str());
  native_draws_ = fallback_draws_ = 0;
  fallback_reasons_.clear();
}

}  // namespace rex::graphics::vulkan::mgr_native
