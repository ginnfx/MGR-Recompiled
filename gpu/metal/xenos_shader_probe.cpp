#include "host_backend.h"
#include "spirv_to_msl.h"
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>
#include <xxhash.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

using namespace mgr::gpu;

// A read-only probe for the existing Xenos shader cache, not a new shader
// compiler or a change to the game's shader selection. Bounds and hash checks
// follow the Xenos cache's packed XESH v20201219 format.
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("Usage: mgr_xenos_msl_probe INPUT.xsh OUTPUT_DIRECTORY");
    std::ifstream input(argv[1], std::ios::binary);
    auto read = [&](auto& value) {
      input.read(reinterpret_cast<char*>(&value), sizeof(value));
      if (!input) throw std::runtime_error("Truncated Xenos shader cache");
    };
    uint32_t magic, version; read(magic); read(version);
    if (magic != 0x48534558 || version != 0x19122020) throw std::runtime_error("Unsupported Xenos cache header");
    std::filesystem::create_directories(argv[2]);
    rex::graphics::SpirvShaderTranslator::Features features(false);
    features.spirv_version = 0x00010300;
    features.max_storage_buffer_range = 512 * 1024 * 1024;
    features.full_draw_index_uint32 = true;
    features.image_view_format_swizzle = true;
    // Conservative host-target path. Fragment interlock, sample-rate shading,
    // geometry and memexport capabilities are not silently assumed.
    rex::graphics::SpirvShaderTranslator translator(features, false, false, false);
    uint32_t passed = 0, failed = 0;
    for (uint32_t n = 0; n < 16 && input.peek() != std::char_traits<char>::eof(); ++n) {
      uint64_t hash; uint32_t packed; read(hash); read(packed);
      const uint32_t count = packed & 0x7fffffff;
      if (!count || count > 65535) throw std::runtime_error("Invalid microcode length");
      std::vector<uint32_t> code(count); input.read(reinterpret_cast<char*>(code.data()), count * 4);
      if (!input || XXH3_64bits(code.data(), count * 4) != hash) throw std::runtime_error("Corrupt microcode hash");
      const bool pixel = packed >> 31;
      const auto stem = std::filesystem::path(argv[2]) / (std::to_string(hash) + (pixel ? ".frag" : ".vert"));
      try {
        rex::graphics::Shader shader(pixel ? rex::graphics::xenos::ShaderType::kPixel : rex::graphics::xenos::ShaderType::kVertex,
            hash, code.data(), count);
        rex::string::StringBuffer disassembly; shader.AnalyzeUcode(disassembly);
        const auto modification = pixel ? translator.GetDefaultPixelShaderModification(0) : translator.GetDefaultVertexShaderModification(0);
        auto* translated = shader.GetOrCreateTranslation(modification);
        if (!translator.TranslateAnalyzedShader(*translated)) throw std::runtime_error("Xenos translation failed");
        const auto& binary = translated->translated_binary();
        if (binary.size() % 4) throw std::runtime_error("Unaligned SPIR-V");
        ShaderCode spirv; spirv.stage = pixel ? Stage::kFragment : Stage::kVertex;
        spirv.spirv.resize(binary.size() / 4); std::memcpy(spirv.spirv.data(), binary.data(), binary.size());
        std::ofstream spv(stem.string() + ".spv", std::ios::binary);
        spv.write(reinterpret_cast<const char*>(binary.data()), binary.size());
        auto msl = TranslateSpirvToMsl(spirv);
        std::ofstream source(stem.string() + ".metal"); source << msl.source;
        ValidateMslWithMetal(msl);
        ++passed; std::printf("translated %llu %s buffers=%zu\n", static_cast<unsigned long long>(hash), pixel ? "PS" : "VS", msl.buffers.size());
      } catch (const std::exception& e) {
        ++failed; std::printf("unsupported %llu %s: %s\n", static_cast<unsigned long long>(hash), pixel ? "PS" : "VS", e.what());
      }
    }
    std::printf("probe compiled_msl=%u unsupported=%u (library compilation, not game rendering parity)\n", passed, failed);
    return passed ? 0 : 1;
  } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
