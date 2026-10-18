#include "spirv_to_msl.h"
#include <spirv_msl.hpp>
#include <stdexcept>

namespace mgr::gpu {
MslShader TranslateSpirvToMsl(const ShaderCode& shader) {
  if (shader.spirv.size() < 5 || shader.spirv.front() != 0x07230203)
    throw std::runtime_error("Invalid SPIR-V module");
  spirv_cross::CompilerMSL compiler(shader.spirv);
  const auto model = shader.stage == Stage::kVertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
  compiler.set_entry_point(shader.entry, model);
  auto options = compiler.get_msl_options();
  options.platform = spirv_cross::CompilerMSL::Options::macOS;
  options.set_msl_version(3, 0);
  // Use explicit Metal buffer/texture/sampler spaces for Metal 4 argument
  // tables. Vulkan descriptor-set numbers are reflected below, not flattened.
  options.argument_buffers = false;
  compiler.set_msl_options(options);
  auto common = compiler.get_common_options();
  common.force_zero_initialized_variables = true;
  common.vertex.flip_vert_y = shader.flip_y;
  compiler.set_common_options(common);
  MslShader result;
  result.source = compiler.compile();
  result.entry = compiler.get_cleansed_entry_point_name(shader.entry, model);
  const auto resources = compiler.get_shader_resources();
  auto append = [&](const auto& list) {
    for (const auto& r : list) {
      const auto slot = compiler.get_automatic_msl_resource_binding(r.id);
      // Xenos declares common bindings that a particular shader doesn't use.
      // SPIRV-Cross reports UINT32_MAX for those, not a live buffer slot.
      if (slot == UINT32_MAX) continue;
      if (slot >= 31) throw std::runtime_error("SPIR-V buffer bindings exceed initial Metal host interface");
      result.buffers.push_back({compiler.get_decoration(r.id, spv::DecorationDescriptorSet),
          compiler.get_decoration(r.id, spv::DecorationBinding), slot});
    }
  };
  append(resources.uniform_buffers);
  append(resources.storage_buffers);
  auto append_resources = [&](const auto& list, auto& output, uint32_t limit) {
    for (const auto& r : list) {
      const auto slot = compiler.get_automatic_msl_resource_binding(r.id);
      if (slot == UINT32_MAX) continue;
      if (slot >= limit || !compiler.get_type(r.type_id).array.empty()) {
        result.needs_extended_bindings = true;
        continue;
      }
      output.push_back({compiler.get_decoration(r.id, spv::DecorationDescriptorSet),
          compiler.get_decoration(r.id, spv::DecorationBinding), slot});
    }
  };
  append_resources(resources.separate_images, result.textures, 128);
  append_resources(resources.separate_samplers, result.samplers, 16);
  result.needs_extended_bindings |= !resources.push_constant_buffers.empty() || !resources.sampled_images.empty() ||
      !resources.storage_images.empty() || compiler.needs_swizzle_buffer() ||
      compiler.needs_buffer_size_buffer();
  return result;
}
}
