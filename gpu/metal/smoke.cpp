#include "host_backend.h"
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace mgr::gpu;
namespace {
ShaderCode LoadShader(const char* file, Stage stage) {
  auto path = std::filesystem::path(MGR_SMOKE_SHADER_DIR) / file;
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("Missing SPIR-V: " + path.string());
  const auto size = stream.tellg();
  if (size < 20 || size % 4) throw std::runtime_error("Malformed SPIR-V size");
  ShaderCode code; code.stage = stage; code.spirv.resize(size_t(size) / 4);
  stream.seekg(0); stream.read(reinterpret_cast<char*>(code.spirv.data()), size);
  if (!stream) throw std::runtime_error("Read SPIR-V failed");
  return code;
}
void CheckPixel(const std::vector<uint8_t>& bytes, uint32_t width, uint32_t x, uint32_t y,
                std::array<int, 4> expected, const char* test) {
  const auto* p = bytes.data() + (size_t(y) * width + x) * 4;
  for (size_t c = 0; c < 4; ++c) if (std::abs(int(p[c]) - expected[c]) > 2) {
    throw std::runtime_error(std::string(test) + " at " + std::to_string(x) + "," + std::to_string(y) +
        ": got " + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," +
        std::to_string(p[2]) + "," + std::to_string(p[3]));
  }
}
void SavePpm(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, uint32_t width, uint32_t height) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("Cannot write capture");
  out << "P6\n" << width << ' ' << height << "\n255\n";
  for (size_t i = 0; i < bytes.size(); i += 4) out.write(reinterpret_cast<const char*>(bytes.data() + i), 3);
}
}
int main(int argc, char** argv) {
  SDL_Window* window = nullptr;
  try {
    bool show_window = false;
    uint32_t frames = 6;
    std::filesystem::path cache = "private/metal4-cache", capture;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--window") show_window = true;
      else if (arg == "--frames" && i + 1 < argc) frames = std::stoul(argv[++i]);
      else if (arg == "--cache" && i + 1 < argc) cache = argv[++i];
      else if (arg == "--capture" && i + 1 < argc) capture = argv[++i];
      else throw std::runtime_error("Usage: mgr_metal4_smoke [--window] [--frames N] [--cache DIR] [--capture FILE.ppm]");
    }
    if (frames < 4 || frames > 10000) throw std::runtime_error("Frames must be 4..10000 (exercise slot reuse)");
    if (show_window) {
      if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
      window = SDL_CreateWindow("MGR Metal 4 bring-up", 640, 360,
          SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
      if (!window) throw std::runtime_error(SDL_GetError());
    }
    {
      auto backend = CreateMetal4Backend({window, cache, true});
      std::printf("device=%s archive_loaded=%d\n", backend->device_name().c_str(), backend->stats().archive_loaded);
      auto target = backend->CreateTarget({128, 128, Format::kRGBA8, "Smoke RGBA8 color"});
      auto depth = backend->CreateTarget({128, 128, Format::kDepth32Stencil8, "Smoke depth/stencil"});
      PipelineDesc pd;
      pd.vertex = LoadShader("smoke.vert.spv", Stage::kVertex);
      pd.fragment = LoadShader("smoke.frag.spv", Stage::kFragment);
      pd.depth_stencil = true; pd.depth_write = true; pd.depth_compare = Compare::kLess;
      pd.stencil = true; pd.stencil_pass = StencilOp::kReplace;
      pd.blend = true; pd.source_rgb = BlendFactor::kSourceAlpha;
      pd.destination_rgb = BlendFactor::kOneMinusSourceAlpha;
      pd.source_alpha = BlendFactor::kOne; pd.destination_alpha = BlendFactor::kOneMinusSourceAlpha;
      pd.label = "Smoke blended depth-tested SPIR-V pipeline";
      auto pending = backend->PreparePipeline(pd);
      auto duplicate = backend->PreparePipeline(pd);
      auto pipeline = pending.get();
      if (pipeline != duplicate.get()) throw std::runtime_error("Duplicate pipeline was not reused");
      auto stencil_desc = pd;
      stencil_desc.depth_write = false; stencil_desc.depth_compare = Compare::kAlways;
      stencil_desc.stencil_compare = Compare::kEqual; stencil_desc.stencil_pass = StencilOp::kKeep;
      stencil_desc.stencil_write_mask = 0;
      auto stencil_pipeline = backend->PreparePipeline(stencil_desc).get();
      std::shared_ptr<Pipeline> window_pipeline;
      if (window) {
        auto wp = pd; wp.color = Format::kBGRA8; wp.depth_stencil = false;
        wp.depth_write = false; wp.depth_compare = Compare::kAlways; wp.stencil = false;
        window_pipeline = backend->PreparePipeline(wp).get();
      }
      const float positions[][4] = {{-0.8f,-0.8f,0.4f,1}, {0.8f,-0.8f,0.4f,1}, {0,0.8f,0.4f,1},
                                   {-0.8f,-0.8f,0.8f,1}, {0.8f,-0.8f,0.8f,1}, {0,0.8f,0.8f,1}};
      const uint16_t indices[] = {0,1,2};
      auto vertices = backend->CreateBuffer(std::as_bytes(std::span(positions)), "Smoke immutable vertices");
      auto index_buffer = backend->CreateBuffer(std::as_bytes(std::span(indices)), "Smoke uint16 indices");
      uint32_t rendered = 0;
      bool quit = false;
      while (rendered < frames && !quit) {
        if (window) { SDL_Event event; while (SDL_PollEvent(&event)) if (event.type == SDL_EVENT_QUIT) quit = true; }
        if (!backend->BeginFrame()) { SDL_Delay(10); continue; }
        PassDesc pass; pass.color = target; pass.depth_stencil = depth;
        pass.clear_color = {0.1, 0.2, 0.4, 1}; pass.label = "Smoke depth + blend";
        backend->BeginPass(pass);
        DrawDesc draw; draw.pipeline = pipeline; draw.count = 3; draw.indices = index_buffer;
        draw.stencil_reference = 7;
        draw.buffers = {{Stage::kVertex, 0, 0, vertices, 0}};
        backend->Draw(draw);
        // The opaque green triangle behind red must fail depth testing.
        draw.indices.reset(); draw.first = 3; backend->Draw(draw);
        backend->EndPass();
        pass.color_load = Load::kLoad; pass.depth_load = Load::kLoad; pass.stencil_load = Load::kLoad;
        pass.label = "Smoke attachment preservation across passes";
        backend->BeginPass(pass); backend->EndPass();
        if (window) {
          PassDesc wp; wp.label = "Smoke SDL Metal 4 presentation"; wp.clear_color = pass.clear_color;
          backend->BeginPass(wp);
          draw.pipeline = window_pipeline; draw.first = 0; backend->Draw(draw);
          backend->EndPass();
        }
        backend->Submit(); ++rendered;
      }
      backend->WaitIdle();
      auto rgba = backend->Readback(target);
      CheckPixel(rgba, 128, 2, 2, {26, 51, 102, 255}, "Clear/load/store");
      CheckPixel(rgba, 128, 64, 64, {115, 51, 64, 255}, "Indexed draw/blending/depth rejection");
      if (!capture.empty()) SavePpm(capture, rgba, 128, 128);
      for (uint32_t reference : {8u, 7u}) {
        if (!backend->BeginFrame(false)) throw std::runtime_error("Could not begin stencil test");
        PassDesc pass; pass.color = target; pass.depth_stencil = depth;
        pass.color_load = pass.depth_load = pass.stencil_load = Load::kLoad;
        pass.label = "Smoke stencil compare after attachment reload";
        backend->BeginPass(pass);
        DrawDesc draw; draw.pipeline = stencil_pipeline; draw.first = 3; draw.count = 3;
        draw.buffers = {{Stage::kVertex, 0, 0, vertices, 0}}; draw.stencil_reference = reference;
        backend->Draw(draw); backend->EndPass(); backend->Submit();
        rgba = backend->Readback(target);
        CheckPixel(rgba, 128, 64, 64, reference == 8 ? std::array<int,4>{115,51,64,255} : std::array<int,4>{0,255,0,255},
            reference == 8 ? "Stencil rejection" : "Stencil acceptance");
      }
      backend->FlushPipelineCache();
      auto stats = backend->stats();
      std::printf("PASS submissions=%llu draws=%llu pipelines=%llu capture=%s\n",
          static_cast<unsigned long long>(stats.submissions), static_cast<unsigned long long>(stats.draws),
          static_cast<unsigned long long>(stats.pipelines_created), capture.string().c_str());
    }
    if (window) SDL_DestroyWindow(window);
    SDL_Quit(); return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    if (window) SDL_DestroyWindow(window);
    SDL_Quit(); return 1;
  }
}
