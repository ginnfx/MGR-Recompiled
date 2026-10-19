#include "runtime.h"
#include "runtime_shaders.h"
#include "spirv_to_msl.h"
#include "texture_copy.h"
#include "texture_swizzle.h"
#include "primitive_expansion.h"
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/string/buffer.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context_sdl.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>

using namespace mgr::gpu;
using namespace mgr::gpu::metal;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void CheckXenosOutputMerger(HostBackend& host) {
  using T = rex::graphics::SpirvShaderTranslator;
  T::Features features(false); features.spirv_version = 0x00010300;
  features.max_storage_buffer_range = 512 * 1024 * 1024;
  features.fragment_shader_sample_interlock = false; features.sample_rate_shading = true;
  T translator(features, false, false, true);
  const auto binary = translator.CreateDepthOnlyFragmentShader();
  for (uint32_t samples : {1u, 4u}) {
    PipelineDesc pipeline;
    pipeline.vertex.spirv.assign(std::begin(shaders::present_vertex), std::end(shaders::present_vertex));
    pipeline.fragment.stage = Stage::kFragment; pipeline.fragment.spirv.resize(binary.size()/4);
    std::memcpy(pipeline.fragment.spirv.data(), binary.data(), binary.size());
    pipeline.color = Format::kNone; pipeline.color_write_mask = 0; pipeline.raster_samples = samples;
    pipeline.label = "Shared Xenos depth/stencil output merger";
    DrawDesc draw; draw.pipeline = host.PreparePipeline(pipeline).get(); draw.count = 3;
    std::vector<uint32_t> original(2048 * 1280, 0xffffff44);
    auto edram = host.CreateBuffer(std::as_bytes(std::span(original)), "Output merger test EDRAM");
    T::SystemConstants constants{};
    constants.flags = T::kSysFlag_FSIDepthStencil | T::kSysFlag_FSIDepthWrite |
        (7u << T::kSysFlag_FSIDepthPassIfLess_Shift) | ((samples == 4 ? 2u : 0u) << T::kSysFlag_MsaaSamples_Shift);
    constants.edram_32bpp_tile_pitch_dwords_scaled = 1280;
    for (auto& mask : constants.edram_rt_keep_mask) mask[0] = mask[1] = ~0u;
    auto first = host.CreateBuffer(std::as_bytes(std::span(&constants, 1)), "Output merger depth constants");
    constants.flags |= T::kSysFlag_FSIStencilTest;
    constants.edram_stencil_front_reference_masks = constants.edram_stencil_back_reference_masks = 0xffff7e;
    constants.edram_stencil_front_func_ops = constants.edram_stencil_back_func_ops = 7 | (2u << 6);
    auto second = host.CreateBuffer(std::as_bytes(std::span(&constants, 1)), "Output merger stencil constants");
    draw.buffers = {{Stage::kFragment, 0, 1, edram}, {Stage::kFragment, 1, T::kConstantBufferSystem, first}};
    PassDesc pass; pass.attachmentless_width = pass.attachmentless_height = 8;
    pass.raster_samples = samples; pass.label = pipeline.label;
    if (samples == 4) for (const auto& p : rex::graphics::draw_util::kD3D10StandardSamplePositions4x)
      pass.sample_positions.push_back({0.5f+p[0]/16.0f, 0.5f+p[1]/16.0f});
    Check(host.BeginFrame(false), "output merger frame"); host.BeginPass(pass);
    host.Draw(draw); draw.buffers[1].buffer = second; host.Draw(draw);
    host.EndPass(); host.Submit();
    const auto bytes = host.ReadBuffer(edram);
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) for (uint32_t sample = 0; sample < samples; ++sample) {
      const uint32_t sx = samples == 4 ? x*2 + (sample>>1) : x;
      const uint32_t sy = samples == 4 ? y*2 + (sample&1) : y;
      const uint32_t offset = sy*80 + sx + 40;
      uint32_t word; std::memcpy(&word, bytes.data() + offset*4, 4);
      Check(word == 0x0000007e, "shared Xenos depth/stencil or raster ordering pixel mismatch");
    }
    uint32_t untouched; std::memcpy(&untouched, bytes.data() + 2000*4, 4);
    Check(untouched == 0xffffff44, "output merger overwrote uncovered EDRAM");
  }
  std::puts("PASS shared Xenos output merger: ordered depth/stencil writes, 1x/4x coverage and EDRAM half-tile addressing");
}
ShaderCode CompositeShaderSpirv() {
  // Nine dwords from the game's fullscreen color shader (4dfdc5afb71c5f74).
  // Previously, the translated alpha-test/alpha-to-coverage merge selected
  // zero coverage when both features were disabled, despite fragment entry.
  constexpr uint32_t code[] = {0x00000000, 0x1001c400, 0x22000000,
      0xc80f8000, 0x00000000, 0x223e3e00, 0, 0, 0};
  rex::graphics::SpirvShader shader(rex::graphics::xenos::ShaderType::kPixel,
      0x4dfdc5afb71c5f74ull, code, std::size(code), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  using T = rex::graphics::SpirvShaderTranslator;
  T::Features features(false); features.spirv_version = 0x00010300;
  features.max_storage_buffer_range = 512 * 1024 * 1024;
  features.fragment_shader_sample_interlock = false;
  features.sample_rate_shading = true;
  T translator(features, false, false, true);
  auto* translation = shader.GetOrCreateTranslation(
      translator.GetDefaultPixelShaderModification(0));
  Check(translator.TranslateAnalyzedShader(*translation) && translation->is_valid(),
        "composite shader translation");
  const auto& binary = translation->translated_binary();
  ShaderCode translated_spirv;
  translated_spirv.stage = Stage::kFragment;
  translated_spirv.spirv.resize(binary.size() / 4);
  std::memcpy(translated_spirv.spirv.data(), binary.data(), binary.size());
  const auto translated_msl = TranslateSpirvToMsl(translated_spirv);
  Check(translated_msl.source.find("coherent device XeEdram& xe_edram") != std::string::npos &&
            translated_msl.source.find("raster_order_group(0)") != std::string::npos,
        "composite EDRAM must preserve coherent raster-order binding in MSL 3.2");
  // The second fullscreen composite shader observed in the game has texture
  // reads and a longer control-flow path. Keep its exact microcode as a
  // translation contract fixture too.
  constexpr uint32_t textured_code[] = {
      0x05746003, 0x10091200, 0x12000001, 0x00000000, 0x600ac400, 0x12000000,
      0x00005010, 0x00002200, 0x00000000, 0xc80c0000, 0x00acac06, 0x8b00bdbd,
      0x10001001, 0x1f1ff888, 0x00004000, 0x30284021, 0x1f1ffff8, 0x00004000,
      0x34284021, 0x1f1fffc7, 0x00004000, 0x38284021, 0x1f1ffe3f, 0x00004000,
      0xb8381001, 0x1f1ff688, 0x00004000, 0x10182001, 0x1f1ff688, 0x00004000,
      0xc8010000, 0x04b11b1b, 0x8b00b9fe, 0xc9070003, 0x006cc000, 0xa100b900,
      0xc80f0000, 0x0000006c, 0x8b01beff, 0xc80f0001, 0x00001b1b, 0x8b00befe,
      0x14120000, 0x00bec01b, 0xb004fe02, 0xa8870005, 0x04b1c000, 0xc00004bb,
      0xc8070000, 0x00b4b4b4, 0xab05ba04, 0xc8070000, 0x00c0b4b4, 0xab00b803,
      0xc8070000, 0x00c0c0b4, 0xab02bb00, 0xc80f0000, 0x00000000, 0xa100bc00,
      0xc80f8000, 0x00000000, 0xe1000100, 0x00000000, 0x00000000, 0x00000000};
  rex::graphics::SpirvShader textured_shader(rex::graphics::xenos::ShaderType::kPixel,
      0xf854bf3b7bf7aa9dull, textured_code, std::size(textured_code), std::endian::native);
  rex::string::StringBuffer textured_disassembly;
  textured_shader.AnalyzeUcode(textured_disassembly);
  auto* textured_translation = textured_shader.GetOrCreateTranslation(
      translator.GetDefaultPixelShaderModification(0));
  Check(translator.TranslateAnalyzedShader(*textured_translation) &&
            textured_translation->is_valid(), "textured composite shader translation");
  ShaderCode textured_spirv;
  textured_spirv.stage = Stage::kFragment;
  const auto& textured_binary = textured_translation->translated_binary();
  textured_spirv.spirv.resize(textured_binary.size() / 4);
  std::memcpy(textured_spirv.spirv.data(), textured_binary.data(), textured_binary.size());
  const auto textured_msl = TranslateSpirvToMsl(textured_spirv);
  Check(textured_msl.source.find("coherent device XeEdram& xe_edram") != std::string::npos &&
            textured_msl.source.find("raster_order_group(0)") != std::string::npos,
        "textured composite EDRAM must preserve coherent raster-order binding in MSL 3.2");
  std::puts("PASS both composite shaders retain coherent MSL 3.2 EDRAM ROG contract");
  return translated_spirv;
}
void CheckCompositeColorMask(HostBackend& host, const ShaderCode& translated_spirv) {
  using T = rex::graphics::SpirvShaderTranslator;
  PipelineDesc pipeline;
  pipeline.vertex.spirv.assign(std::begin(shaders::present_vertex),
                               std::end(shaders::present_vertex));
  pipeline.fragment = translated_spirv;
  pipeline.color = Format::kNone;
  pipeline.label = "Composite color sample-mask regression";
  DrawDesc draw; draw.pipeline = host.PreparePipeline(pipeline).get(); draw.count = 3;
  std::vector<uint32_t> original(2048 * 1280, 0u);
  auto edram = host.CreateBuffer(std::as_bytes(std::span(original)), "Composite test EDRAM");
  T::SystemConstants constants{};
  constants.flags = 0x00078c00u;  // Guest alpha test always, alpha-to-mask disabled.
  constants.edram_32bpp_tile_pitch_dwords_scaled = 1280;
  constants.edram_rt_format_flags[0] = 96;  // RGBA8.
  constants.edram_rt_blend_factors_ops[0] = 0x00010001u;
  constants.color_exp_bias[0] = 1.0f;
  constants.edram_rt_clamp[0][0] = constants.edram_rt_clamp[0][1] = 0.0f;
  constants.edram_rt_clamp[0][2] = constants.edram_rt_clamp[0][3] = 1.0f;
  auto system = host.CreateBuffer(std::as_bytes(std::span(&constants, 1)),
                                  "Composite test system constants");
  const std::array<float, 4> color{0.25f, 0.5f, 0.75f, 1.0f};
  auto floats = host.CreateBuffer(std::as_bytes(std::span(color)),
                                  "Composite test color constants");
  draw.buffers = {{Stage::kFragment, 0, 1, edram},
                  {Stage::kFragment, 1, T::kConstantBufferSystem, system},
                  {Stage::kFragment, 1, T::kConstantBufferFloatPixel, floats}};
  PassDesc pass; pass.attachmentless_width = pass.attachmentless_height = 8;
  pass.label = pipeline.label;
  Check(host.BeginFrame(false), "composite test frame");
  host.BeginPass(pass); host.Draw(draw); host.EndPass(); host.Submit();
  const auto bytes = host.ReadBuffer(edram);
  for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
    uint32_t word;
    std::memcpy(&word, bytes.data() + ((y * 80 + x + 40) * 4), 4);
    Check(word == 0xffbf8040u, "composite shader lost color coverage with alpha features disabled");
  }
  std::puts("PASS composite color sample-mask regression");
}
void CheckEdramResolveGpu(HostBackend& host) {
  constexpr uint32_t width = 64, height = 64;
  constexpr size_t edram_size = 2048 * 5120;
  constexpr size_t guest_size = 0x30000;
  std::vector<std::byte> edram_bytes(edram_size);
  // Color source at base 0: 80x16 tiles, one tile row per 16 pixels.
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const uint32_t tile = (y / 16) & 2047;
    const uint32_t word = (x * 3) | ((y * 5) << 8) | (((x + y) * 7 & 0xff) << 16) | (0xffu << 24);
    std::memcpy(edram_bytes.data() + tile * 5120 + ((y % 16) * 80 + x) * 4, &word, 4);
  }
  // Depth source at base 64, with the depth half-tile column rotation.
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const uint32_t column = x < 40 ? x + 40 : x - 40;
    const uint32_t tile = (64 + (y / 16)) & 2047;
    const uint32_t word = 0x3f800000u + x * 17 + y * 131;
    std::memcpy(edram_bytes.data() + tile * 5120 + ((y % 16) * 80 + column) * 4, &word, 4);
  }
  // 4x MSAA source at base 128, pitch 2; each sample has distinct channels.
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
    for (uint32_t s = 0; s < 4; ++s) {
      const uint32_t sx = (x << 1) + (s >> 1), sy = (y << 1) + (s & 1);
      const uint32_t tile = (128 + (sy / 16) * 2 + sx / 80) & 2047;
      const uint32_t word = uint32_t(10 + x + s * 9) | (uint32_t(20 + y + s * 7) << 8) |
          (uint32_t(30 + x + y + s * 5) << 16) | (0xffu << 24);
      std::memcpy(edram_bytes.data() + tile * 5120 + ((sy % 16) * 80 + sx % 80) * 4, &word, 4);
    }
  auto edram = host.CreateBuffer(std::as_bytes(std::span(edram_bytes)), "GPU resolve test EDRAM");
  auto guest = host.CreateBufferSized(guest_size, "GPU resolve test guest memory");

  auto run = [&](uint32_t base, uint32_t pitch, uint32_t dest, uint32_t endian, uint32_t swap,
                 uint32_t samples, uint32_t x_shift, uint32_t y_shift, uint32_t depth) {
    EdramResolveParams params{};
    params.width = width; params.height = height;
    params.src_base_tiles = base; params.src_pitch_tiles = pitch;
    params.dest_base = dest; params.dest_pitch = width; params.dest_size = guest_size;
    params.endian_xor = endian; params.swap_rb = swap;
    params.sample_count = samples; params.first_sample = 0;
    params.x_shift = x_shift; params.y_shift = y_shift; params.is_depth = depth;
    host.ResolveEdram(edram, guest, params);
  };
  run(0, 1, 0, 0, 1, 1, 0, 0, 0);
  run(64, 1, 0x10000, 3, 0, 1, 0, 0, 1);
  run(128, 2, 0x20000, 0, 1, 4, 1, 1, 0);
  const auto resolved = host.ReadBufferRange(guest, 0, guest_size);
  auto dest_word = [&](uint32_t base, uint32_t x, uint32_t y) {
    const uint32_t offset = uint32_t(rex::graphics::texture_util::GetTiledOffset2D(
        int32_t(x), int32_t(y), width, 2));
    uint32_t word; std::memcpy(&word, resolved.data() + base + offset, 4); return word;
  };
  auto swap_rb = [](uint32_t w) { return (w & 0xff00ff00u) | ((w & 0xffu) << 16) | ((w >> 16) & 0xffu); };
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
    const uint32_t color = (x * 3) | ((y * 5) << 8) | (((x + y) * 7 & 0xff) << 16) | (0xffu << 24);
    const uint32_t color_actual = dest_word(0, x, y);
    if (color_actual != swap_rb(color)) {
      std::fprintf(stderr, "color mismatch at %u,%u expected %08x got %08x\n",
                   x, y, swap_rb(color), color_actual);
      Check(false, "GPU color resolve mismatch");
    }
    const uint32_t depth_value = 0x3f800000u + x * 17 + y * 131;
    const uint32_t depth_actual = dest_word(0x10000, x, y);
    if (depth_actual != __builtin_bswap32(depth_value)) {
      std::fprintf(stderr, "depth mismatch at %u,%u expected %08x got %08x\n",
                   x, y, __builtin_bswap32(depth_value), depth_actual);
      Check(false, "GPU depth resolve mismatch");
    }
    uint32_t sum[4] = {0, 0, 0, 0};
    for (uint32_t s = 0; s < 4; ++s) {
      sum[0] += 10 + x + s * 9; sum[1] += 20 + y + s * 7;
      sum[2] += 30 + x + y + s * 5; sum[3] += 255;
    }
    const uint32_t msaa = uint32_t(std::rint(float(sum[0]) / 4.0f)) |
        (uint32_t(std::rint(float(sum[1]) / 4.0f)) << 8) |
        (uint32_t(std::rint(float(sum[2]) / 4.0f)) << 16) |
        (uint32_t(std::rint(float(sum[3]) / 4.0f)) << 24);
    Check(dest_word(0x20000, x, y) == swap_rb(msaa), "GPU MSAA resolve mismatch");
  }
  std::puts("PASS GPU EDRAM resolve matches CPU oracle: color, depth, 4x average and byte order");
}
void CheckGuestTextureUpload(HostBackend& host) {
  constexpr uint32_t width = 1280, height = 720;
  std::vector<std::byte> bytes(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t i = (size_t(y) * width + x) * 4;
      bytes[i] = std::byte(40); bytes[i + 1] = std::byte(90);
      bytes[i + 2] = std::byte(130); bytes[i + 3] = std::byte(255);
    }
  }
  std::vector<TextureLevel> levels{{width, height, bytes}};
  for (uint32_t iteration = 0; iteration < 4; ++iteration) {
    TargetDesc desc{width, height, Format::kRGBA8,
        "Guest texture upload check " + std::to_string(iteration), true, 1};
    auto texture = host.CreateTexture2D(desc, levels);
    auto pixels = host.Readback(texture);
    size_t nonzero = 0;
    for (size_t i = 0; i + 4 <= pixels.size(); i += 4)
      nonzero += (pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0;
    std::printf("guest texture upload %u: %zu/%u nonblack\n", iteration, nonzero, width * height);
    if (nonzero == 0) throw std::runtime_error("guest texture upload readback is black");
  }
  std::printf("PASS guest texture uploads\n");
}
class FrontendHarness final : public MetalCommandProcessor {
 public:
  explicit FrontendHarness(MetalGraphicsSystem* graphics) : MetalCommandProcessor(graphics, nullptr) {}
  void Start() { Check(SetupContext(), "native CP setup"); }
  void Stop() { ShutdownContext(); }
  bool Feed(std::vector<uint32_t> words) {
    for (auto& word : words) word = rex::byte_swap(word);
    std::vector<uint8_t> storage((words.size() + 1) * 4);
    std::memcpy(storage.data(), words.data(), words.size() * 4);
    rex::memory::RingBuffer reader(storage.data(), storage.size()); reader.AdvanceWrite(words.size() * 4);
    while (reader.read_count()) if (!ExecutePacket(&reader)) return false;
    return true;
  }
  void RejectDraw() { IssueDraw(rex::graphics::xenos::PrimitiveType::kTriangleList, 3, nullptr, false); }
};
class CheckUI final : public rex::ui::UIDrawer {
 public:
  explicit CheckUI(rex::ui::ImmediateDrawer& drawer) : drawer_(drawer) {
    const uint8_t pixels[] = {255,255,255,255, 255,0,0,255, 0,255,0,255, 0,0,255,255};
    texture_ = drawer_.CreateTexture(2, 2, rex::ui::ImmediateTextureFilter::kNearest, false, pixels);
    Check(bool(texture_), "host UI texture creation");
  }
  void Draw(rex::ui::UIDrawContext& context) override {
    const rex::ui::ImmediateVertex v[] = {
        {4,4,0,0,0xffffffff}, {60,4,1,0,0xffffffff}, {4,60,0,1,0xffffffff}};
    const uint16_t indices[] = {0,1,2};
    drawer_.Begin(context, float(context.render_target_width()), float(context.render_target_height()));
    drawer_.BeginDrawBatch({v, 3, indices, 3});
    rex::ui::ImmediateDraw draw; draw.count = 3; draw.texture = texture_.get();
    draw.scissor = true; draw.scissor_left = 4; draw.scissor_top = 4; draw.scissor_right = 60; draw.scissor_bottom = 60;
    drawer_.Draw(draw); drawer_.EndDrawBatch(); drawer_.End(); ++draws;
  }
  uint32_t draws = 0;
 private:
  rex::ui::ImmediateDrawer& drawer_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
};
}
void CheckBlockCompressedTextureCopy() {
  std::array<uint8_t, 16> source;
  for (size_t i = 0; i < source.size(); ++i)
    source[i] = uint8_t(i * 37 + 11);
  std::array<uint8_t, 16> destination{};
  constexpr uint32_t lane_xor[] = {0, 1, 3, 2};
  for (uint32_t bytes : {8u, 16u}) {
    for (uint32_t endian = 0; endian < 4; ++endian) {
      mgr::metal4::CopyGuestTextureBlock(true, endian, source.data(), destination.data(), bytes);
      for (size_t word = 0; word < bytes; word += 4)
        for (size_t byte = 0; byte < 4; ++byte)
          Check(destination[word + byte] == source[word + (byte ^ lane_xor[endian])],
                "BC block endian swap must match Vulkan per 32-bit word");
    }
  }
  std::array<uint8_t, 4> rgba{1, 2, 3, 4}, permuted{};
  mgr::metal4::CopyGuestTextureBlock(false, 2, rgba.data(), permuted.data(),
                                     uint32_t(rgba.size()));
  Check(permuted == std::array<uint8_t, 4>{4, 3, 2, 1},
        "uncompressed endianness permutation");
  std::puts("PASS BC blocks and uncompressed pixels apply Xenos endianness");
}
void CheckGuestTextureSwizzle() {
  using namespace rex::graphics::xenos;
  const uint32_t rgba = XE_GPU_TEXTURE_SWIZZLE_RGBA;
  Check(mgr::metal4::ShaderTextureSwizzle(TextureFormat::k_24_8_FLOAT, rgba) ==
            XE_GPU_TEXTURE_SWIZZLE_RRRR,
        "depth float fetch must replicate red as Vulkan does");
  Check(mgr::metal4::ShaderTextureSwizzle(TextureFormat::k_DXN, rgba) ==
            XE_GPU_TEXTURE_SWIZZLE_RGGG,
        "BC5 fetch must replicate green as Vulkan does");
  Check(mgr::metal4::ShaderTextureSwizzle(TextureFormat::k_8_8_8_8, rgba) == rgba,
        "RGBA fetch must retain all four channels");
  std::puts("PASS guest texture format swizzles match Vulkan for depth, BC5 and RGBA");
}
void CheckTriangleFanExpansion() {
  const std::array<uint32_t, 4> vertices{7, 9, 11, 13};
  const std::vector<uint32_t> expected{7, 9, 11, 7, 11, 13};
  Check(mgr::metal4::TriangleFanIndices(vertices) == expected,
        "triangle fan must expand to the Vulkan-equivalent triangle list");
  std::puts("PASS guest triangle fan expands to a triangle list");
}

int main(int argc, char** argv) {
  try {
    bool window_test = false, reject_draw = false, translation_only = false;
    std::filesystem::path shader_cache;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--window") window_test = true;
      else if (arg == "--reject-draw") reject_draw = true;
      else if (arg == "--translation-only") translation_only = true;
      else if (arg == "--shader-cache" && i + 1 < argc) shader_cache = argv[++i];
      else throw std::runtime_error("Unknown integration check argument");
    }
    // Keep source translation checks runnable on machines without Metal 4.
    const auto composite_shader_spirv = CompositeShaderSpirv();
    CheckBlockCompressedTextureCopy();
    CheckGuestTextureSwizzle();
    CheckTriangleFanExpansion();
    if (translation_only) return 0;
    // Tests the actual plugin ABI and loader, not a directly constructed
    // replacement that could accidentally bypass runtime backend selection.
    auto plugin = rex::system::LoadGpuPlugin("xenos-metal4", "metal4");
    Check(bool(plugin), "native plugin load/ABI");
    Check(plugin->SetupPresentation(nullptr) == 0, "native presentation setup");
    Check(plugin->presenter() && plugin->provider(), "native provider/presenter services");
    auto* provider = static_cast<MetalProvider*>(plugin->provider());
    auto context = provider->context();
    auto* presenter = plugin->presenter();
    std::vector<std::byte> pixels(16 * 8 * 4);
    for (size_t y = 0; y < 8; ++y) for (size_t x = 0; x < 16; ++x) {
      const auto i = (y * 16 + x) * 4;
      pixels[i] = std::byte(x < 8 ? 255 : 0); pixels[i+1] = std::byte(y < 4 ? 255 : 0);
      pixels[i+2] = std::byte(x >= 8 ? 255 : 0); pixels[i+3] = std::byte(255);
    }
    CheckXenosOutputMerger(*context->backend);
    CheckCompositeColorMask(*context->backend, composite_shader_spirv);
    CheckGuestTextureUpload(*context->backend);
    CheckEdramResolveGpu(*context->backend);
    std::shared_ptr<Target> image;
    { std::lock_guard lock(context->mutex);
      image = context->backend->CreateTextureRGBA8(16, 8, pixels, "Native runtime mailbox checker"); }
    for (int n = 0; n < 8; ++n) Check(presenter->RefreshGuestOutput(16, 8, 2, 1,
        [&](auto& output) { auto& native = static_cast<MetalRefreshContext&>(output); native.image = image;
          native.SetIs8bpc(true); return true; }), "native mailbox refresh");
    rex::ui::RawImage captured;
    Check(presenter->CaptureGuestOutput(captured), "native mailbox capture");
    Check(captured.width == 16 && captured.height == 8 && captured.data.size() == pixels.size() &&
        std::memcmp(captured.data.data(), pixels.data(), pixels.size()) == 0, "native captured pixel identity");
    if (window_test) {
      rex::ui::SDLWindowedAppContext app;
      Check(app.Initialize(), "SDK SDL context");
      rex::ui::WindowSDL window(app, "MGR native runtime integration", 640, 360);
      Check(window.Open(), "SDK SDL window");
      auto drawer = plugin->provider()->CreateImmediateDrawer();
      Check(bool(drawer), "native host UI drawer"); drawer->SetPresenter(presenter);
      CheckUI ui(*drawer); presenter->AddUIDrawerFromUIThread(&ui, 1);
      window.SetPresenter(presenter);
      for (int i = 0; i < 20; ++i) {
        SDL_PumpEvents(); presenter->PaintFromUIThread(true);
        if (i == 8) window.SetDesiredLogicalSize(800, 450);
      }
      Check(ui.draws > 0, "native overlay did not execute");
      presenter->RemoveUIDrawerFromUIThread(&ui); window.SetPresenter(nullptr);
      drawer->SetPresenter(nullptr); drawer.reset(); window.RequestClose();
      std::printf("PASS SDK SDL presentation/resize and textured indexed host UI draws=%u\n", ui.draws);
    }
    plugin->Shutdown(); plugin.reset();

    MetalGraphicsSystem graphics;
    Check(graphics.SetupPresentation(nullptr) == 0, "shared frontend provider setup");
    FrontendHarness frontend(&graphics); frontend.Start();
    const uint32_t reg = rex::graphics::XE_GPU_REG_RB_COLOR_MASK;
    Check(frontend.Feed({reg, 15}), "shared PM4 type-0 register packet");
    Check((*graphics.register_file())[reg] == 15, "PM4 did not update shared Xenos register file");
    if (!shader_cache.empty()) {
      std::ifstream input(shader_cache, std::ios::binary);
      uint32_t magic, version, packed; uint64_t hash;
      input.read(reinterpret_cast<char*>(&magic), 4); input.read(reinterpret_cast<char*>(&version), 4);
      input.read(reinterpret_cast<char*>(&hash), 8); input.read(reinterpret_cast<char*>(&packed), 4);
      Check(input && magic == 0x48534558 && version == 0x19122020, "shader cache fixture header");
      const uint32_t count = packed & 0x7fffffff;
      Check(count && count < 16380, "shader fixture size");
      std::vector<uint32_t> words(count);
      input.read(reinterpret_cast<char*>(words.data()), size_t(count) * 4); Check(bool(input), "shader fixture data");
      const uint32_t opcode = rex::graphics::xenos::PM4_IM_LOAD_IMMEDIATE;
      std::vector<uint32_t> packet = {0xc0000000u | ((count + 1) << 16) | (opcode << 8), packed >> 31, count};
      for (auto word : words) packet.push_back(rex::byte_swap(word));
      Check(frontend.Feed(packet) && frontend.Feed(packet), "shared PM4 inline shader loading");
      auto* shader = packed >> 31 ? frontend.active_pixel_shader() : frontend.active_vertex_shader();
      Check(shader && shader->ucode_data_hash() == hash && frontend.frontend_stats().shader_loads == 1,
          "native frontend shader identity/reuse");
    }
    // Actual no-work XE_SWAP packet through the shared PM4 dispatch.
    const auto swap = uint32_t(rex::graphics::xenos::PM4_XE_SWAP);
    Check(frontend.Feed({0xc0000000u | (3u << 16) | (swap << 8),
        rex::graphics::xenos::kSwapSignature, 0, 0, 0}), "shared PM4 swap dispatch");
    Check(frontend.frontend_stats().swaps == 1, "PM4 did not call Metal IssueSwap");
    if (reject_draw) frontend.RejectDraw();
    frontend.Stop(); graphics.Shutdown();
    std::printf("PASS native plugin ABI, mailbox capture, shared PM4 registers and swap dispatch\n");
    return 0;
  } catch (const std::exception& e) { std::fprintf(stderr, "FAIL native runtime integration: %s\n", e.what()); return 1; }
}
