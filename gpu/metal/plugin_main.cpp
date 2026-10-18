#include "runtime.h"
#include <rex/system/gpu_plugin.h>
#include <rex/logging.h>
#include <string_view>

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version() {
  return rex::system::kGpuPluginAbiVersion;
}
extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t version, const rex::system::GpuCreateInfo* info) {
  if (version != rex::system::kGpuPluginAbiVersion || !info || info->struct_size < sizeof(*info)) return nullptr;
  const std::string_view backend = info->backend ? info->backend : "any";
  if (backend != "any" && backend != "metal" && backend != "metal4") {
    REXLOG_ERROR("xenos-metal4: unsupported backend '{}'", backend); return nullptr;
  }
  return new mgr::gpu::metal::MetalGraphicsSystem();
}
