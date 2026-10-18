# In-tree build of the ReXGlue Xenos GPU plugin (rexgpu-xenos) from
# vendor/rexglue-sdk (commit bd833a2, the shipped SDK binary's source), linked
# against the installed SDK runtime. rexglue_setup_target(... GPU_PLUGINS xenos)
# prefers this target over the prebuilt rex::gpu-xenos, so MGR-specific GPU
# fixes/optimizations live here. Local changes: `git -C vendor/rexglue-sdk diff`.

set(REXSDK_SRC ${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk)
set(XENOS_SRC ${REXSDK_SRC}/src/graphics)
if(NOT EXISTS ${XENOS_SRC}/plugin_main.cpp)
    message(FATAL_ERROR "Missing ${XENOS_SRC}; see PROGRESS.md (vendor setup)")
endif()

set(XENOS_SOURCES
    plugin_main.cpp flags.cpp register_file.cpp registers.cpp
    pipeline/shader/shader.cpp format/ucode.cpp sampler_info.cpp
    graphics_system.cpp command_processor.cpp util/draw_extent_estimator.cpp
    util/draw.cpp packet_disassembler.cpp primitive_processor.cpp
    pipeline/render_target/cache.cpp shared_memory.cpp pipeline/texture/cache.cpp
    pipeline/shader/interpreter.cpp pipeline/shader/translator.cpp
    pipeline/shader/translator_disasm.cpp
    pipeline/shader/spirv_builder.cpp pipeline/shader/spirv.cpp
    pipeline/shader/spirv_translator.cpp pipeline/shader/spirv_translator_alu.cpp
    pipeline/shader/spirv_translator_fetch.cpp
    pipeline/shader/spirv_translator_memexport.cpp
    pipeline/shader/spirv_translator_rb.cpp
    vulkan/graphics_system.cpp vulkan/deferred_command_buffer.cpp
    vulkan/command_processor.cpp vulkan/pipeline_cache.cpp
    vulkan/primitive_processor.cpp vulkan/render_target_cache.cpp
    vulkan/shader.cpp vulkan/shared_memory.cpp vulkan/texture_cache.cpp
)
if(EXISTS "${XENOS_SRC}/vulkan/shader_features.cpp")
    list(APPEND XENOS_SOURCES vulkan/shader_features.cpp)
endif()
list(TRANSFORM XENOS_SOURCES PREPEND ${XENOS_SRC}/)
list(APPEND XENOS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mgr_cmd_stats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mgr_native_ps.cpp)

add_library(rexgpu-xenos SHARED ${XENOS_SOURCES})
set_target_properties(rexgpu-xenos PROPERTIES
    # Host loads lib<name><suffix>.dylib with the SDK's per-config suffix.
    OUTPUT_NAME_DEBUG rexgpu-xenosd
    OUTPUT_NAME_RELWITHDEBINFO rexgpu-xenosrd
    OUTPUT_NAME_RELEASE rexgpu-xenos
    # Build straight into the exe dir: the SDK's POST_BUILD staging only runs
    # when mgr relinks, which left a stale plugin after plugin-only rebuilds.
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(rexgpu-xenos BEFORE PRIVATE
    ${REXSDK_SRC}/include  # vendored headers (incl. MGR edits) win over the SDK copy
    ${REXSDK_SRC}
    ${REXSDK_SRC}/src
    ${REXSDK_SRC}/thirdparty/renderdoc
    ${CMAKE_SOURCE_DIR}/vendor/glslang
)
target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable -Wno-deprecated-declarations)
target_link_libraries(rexgpu-xenos PRIVATE
    rex::runtime
    rex::xxhash
    rex::SPIRV
    rex::glslang
)
