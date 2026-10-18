#include "guest_renderer.h"
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <xxhash.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace mgr::gpu::metal {
MetalCommandProcessor::MetalCommandProcessor(MetalGraphicsSystem* graphics, rex::system::KernelState* kernel)
    : CommandProcessor(graphics, kernel), context_(graphics->metal_provider()->context()) {}
MetalCommandProcessor::~MetalCommandProcessor() = default;
bool MetalCommandProcessor::SetupContext() {
  std::lock_guard lock(context_->mutex);
  REXLOG_INFO("Metal Xenos command processor connected: shared PM4/register frontend, device {}",
      context_->backend->device_name());
  return CommandProcessor::SetupContext();
}
void MetalCommandProcessor::ShutdownContext() {
  std::lock_guard lock(context_->mutex); context_->backend->WaitIdle();
  context_->backend->FlushPipelineCache(); guest_.reset(); shaders_.clear(); CommandProcessor::ShutdownContext();
}
void MetalCommandProcessor::InitializeShaderStorage(const std::filesystem::path&, uint32_t, bool) {
  // Host pipelines are cached by the backend. Guest microcode remains owned by
  // the shared Shader objects; do not append to the Vulkan storage format.
}
rex::graphics::Shader* MetalCommandProcessor::LoadShader(rex::graphics::xenos::ShaderType type,
    uint32_t, const uint32_t* code, uint32_t count) {
  if (!code || !count || count > 65535) Unsupported("invalid guest shader microcode");
  const auto hash = XXH3_64bits(code, size_t(count) * 4);
  auto key = std::make_pair(type, hash);
  auto& shader = shaders_[key];
  if (!shader) {
    shader = std::make_unique<rex::graphics::SpirvShader>(type, hash, code, count);
    rex::string::StringBuffer disassembly; shader->AnalyzeUcode(disassembly); ++stats_.shader_loads;
  }
  return shader.get();
}
[[noreturn]] void MetalCommandProcessor::Unsupported(const char* operation) {
  // The common packet worker has no exception boundary. Fail explicitly rather
  // than returning success for unrendered draws or letting the guest hang on
  // a resolve/query result that was never produced.
  std::fprintf(stderr, "[metal4-xenos] unsupported guest operation: %s. Native guest rendering is incomplete; use --gpu_plugin=xenos for the working renderer.\n", operation);
  std::fflush(stderr);
  try { context_->backend->FlushPipelineCache(); } catch (...) {}
  std::exit(2);
}
bool MetalCommandProcessor::IssueDraw(rex::graphics::xenos::PrimitiveType primitive, uint32_t count, IndexBufferInfo* indices, bool) {
  if (!count) return true;
  std::lock_guard lock(context_->mutex);
  try {
    if (!memory_) throw std::runtime_error("Xenos draw has no guest physical memory");
    if (!guest_) guest_ = std::make_unique<GuestRenderer>(*context_->backend, *register_file_, *memory_);
    guest_->Draw(primitive, count, indices, static_cast<rex::graphics::SpirvShader*>(active_vertex_shader()), static_cast<rex::graphics::SpirvShader*>(active_pixel_shader()));
    ++stats_.draws; return true;
  } catch (const std::exception& e) { ++stats_.unsupported_draws;
    std::fprintf(stderr, "[metal4-xenos] draw=%llu prim=%u count=%u mode=%08x depth=%08x color=%08x pitch=%08x vs=%p ps=%p\n", (unsigned long long)stats_.draws, uint32_t(primitive), count, (*register_file_)[rex::graphics::XE_GPU_REG_RB_MODECONTROL], (*register_file_)[rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL], (*register_file_)[rex::graphics::XE_GPU_REG_RB_COLOR_INFO], (*register_file_)[rex::graphics::XE_GPU_REG_RB_SURFACE_INFO], active_vertex_shader(), active_pixel_shader());
    Unsupported(e.what()); }
}
bool MetalCommandProcessor::IssueCopy() {
  std::lock_guard lock(context_->mutex);
  try {
    if (!memory_) throw std::runtime_error("Xenos resolve has no guest physical memory");
    if (!guest_) guest_ = std::make_unique<GuestRenderer>(*context_->backend, *register_file_, *memory_);
    guest_->Resolve(); ++stats_.resolves; return true;
  } catch (const std::exception& e) { ++stats_.unsupported_resolves; Unsupported(e.what()); }
}
void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t width, uint32_t height) {
  ++stats_.swaps;
  if ((!width || !height) && !memory_) {
    if (auto* p = graphics_system_->presenter()) p->RefreshGuestOutput(0, 0, 0, 0, [](auto&) { return true; });
    return;
  }
  try {
    std::shared_ptr<Target> image;
    { std::lock_guard lock(context_->mutex);
      if (!memory_) throw std::runtime_error("Xenos swap has no guest physical memory");
      const auto fetch = register_file_->GetTextureFetch(0);
      if (!width) width = fetch.size_2d.width + 1;
      if (!height) height = fetch.size_2d.height + 1;
      if (!guest_) guest_ = std::make_unique<GuestRenderer>(*context_->backend, *register_file_, *memory_);
      const auto fetch0 = register_file_->GetTextureFetch(0);
      if (stats_.swaps <= 6 || stats_.swaps % 10 == 0)
        std::fprintf(stderr,
            "[metal4-xenos] swap packet=%llu ptr=%08x %ux%u fetch0=%08x %ux%u draws=%llu resolves=%llu\n",
            static_cast<unsigned long long>(stats_.swaps), frontbuffer_ptr, width, height,
            uint32_t(fetch0.base_address) << 12, fetch0.size_2d.width + 1,
            fetch0.size_2d.height + 1, static_cast<unsigned long long>(stats_.draws),
            static_cast<unsigned long long>(stats_.resolves));
      image = guest_->FrontBuffer(width, height);
      if (stats_.swaps <= 3 || stats_.swaps % 10 == 0)
        std::fprintf(stderr,
            "[metal4-xenos] guest present %ux%u swap=%llu draws=%llu resolves=%llu host_draws=%llu "
            "unsupported=%llu/%llu pipelines=%llu submissions=%llu archive=%d "
            "commit_us=%llu submit_us=%llu wait_us=%llu res_add=%llu "
            "draw_us=%llu resolve_us=%llu front_us=%llu "
            "gpu_resolves=%llu gpu_resolve_us=%llu gpu_dispatch_us=%llu "
            "gpu_commit_us=%llu gpu_wait_us=%llu "
            "texture_us=%llu texture_calls=%llu texture_uploads=%llu "
            "tex_lookup_us=%llu tex_translate_us=%llu tex_convert_us=%llu "
            "tex_upload_us=%llu tex_wait_us=%llu tex_bytes=%llu tex_unique=%llu "
            "tex_hits=%llu tex_misses=%llu tex_hashes=%llu tex_hash_bytes=%llu "
            "pipe_compile_us=%llu pipe_compiles=%llu\n",
            width, height, (unsigned long long)stats_.swaps, (unsigned long long)stats_.draws,
            (unsigned long long)stats_.resolves, (unsigned long long)context_->backend->stats().draws,
            (unsigned long long)stats_.unsupported_draws,
            (unsigned long long)stats_.unsupported_resolves,
            (unsigned long long)context_->backend->stats().pipelines_created,
            (unsigned long long)context_->backend->stats().submissions,
            context_->backend->stats().archive_loaded ? 1 : 0,
            (unsigned long long)context_->backend->stats().residency_commit_us,
            (unsigned long long)context_->backend->stats().queue_submit_us,
            (unsigned long long)context_->backend->stats().wait_us,
            (unsigned long long)context_->backend->stats().residency_additions,
            (unsigned long long)guest_->draw_us, (unsigned long long)guest_->resolve_us,
            (unsigned long long)guest_->frontbuffer_us,
            (unsigned long long)guest_->resolve_gpu_count,
            (unsigned long long)guest_->resolve_gpu_us,
            (unsigned long long)context_->backend->stats().resolve_dispatch_us,
            (unsigned long long)context_->backend->stats().resolve_commit_us,
            (unsigned long long)context_->backend->stats().resolve_wait_us,
            (unsigned long long)guest_->texture_us, (unsigned long long)guest_->texture_calls,
            (unsigned long long)guest_->texture_uploads,
            (unsigned long long)guest_->texture_lookup_us,
            (unsigned long long)guest_->texture_translate_us,
            (unsigned long long)guest_->texture_convert_us,
            (unsigned long long)guest_->texture_upload_us,
            (unsigned long long)guest_->texture_wait_us,
            (unsigned long long)guest_->texture_bytes,
            (unsigned long long)guest_->texture_unique,
            (unsigned long long)guest_->texture_cache_hits,
            (unsigned long long)guest_->texture_cache_misses,
            (unsigned long long)guest_->texture_hashes,
            (unsigned long long)guest_->texture_hash_bytes,
            (unsigned long long)context_->backend->stats().pipeline_compile_us,
            (unsigned long long)context_->backend->stats().pipeline_compiles);
    }
    if (auto* p = graphics_system_->presenter()) p->RefreshGuestOutput(width, height, width, height,
        [image](auto& refresh) { static_cast<MetalRefreshContext&>(refresh).image = image; return true; });
  } catch (const std::exception& e) { Unsupported(e.what()); }
}
}  // namespace mgr::gpu::metal
