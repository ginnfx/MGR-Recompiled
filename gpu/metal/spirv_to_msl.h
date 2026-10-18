#pragma once
#include "host_backend.h"

namespace mgr::gpu {
struct MslBinding {
  uint32_t descriptor_set, binding, metal_index;
};
struct MslShader {
  std::string source, entry;
  std::vector<MslBinding> buffers;
  bool needs_extended_bindings = false;
};
// Preserves the existing SPIR-V intermediate. Descriptor-set and binding
// identities are reported explicitly; they are not interchangeable Metal slots.
MslShader TranslateSpirvToMsl(const ShaderCode&);
// Diagnostic compilation without a draw pipeline or resource bindings.
void ValidateMslWithMetal(const MslShader&);
}
