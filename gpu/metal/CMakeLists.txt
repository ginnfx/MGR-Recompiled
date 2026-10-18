if(NOT APPLE)
    message(FATAL_ERROR "MGR Metal 4 requires macOS 26+ on Apple Silicon")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 26.0)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0)
endif()
if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The native Metal 4 targets require CMAKE_OSX_ARCHITECTURES=arm64")
endif()
set(MGR_METAL_CPP_ROOT "${CMAKE_SOURCE_DIR}/vendor/metal-cpp" CACHE PATH "Pinned metal-cpp checkout")
set(MGR_SPIRV_CROSS_ROOT "${CMAKE_SOURCE_DIR}/vendor/spirv-cross" CACHE PATH "Pinned SPIRV-Cross checkout")
if(NOT EXISTS "${MGR_METAL_CPP_ROOT}/Metal/Metal.hpp" OR
   NOT EXISTS "${MGR_SPIRV_CROSS_ROOT}/spirv_msl.hpp")
    message(FATAL_ERROR "Run scripts/setup-metal4.sh to prepare isolated native dependencies")
endif()
find_package(SDL3 REQUIRED CONFIG)
find_package(Threads REQUIRED)
enable_language(OBJCXX)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${MGR_SPIRV_CROSS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/spirv-cross" EXCLUDE_FROM_ALL)

add_library(mgr_metal4 STATIC backend.cpp spirv_to_msl.cpp layer_bridge.mm)
set_source_files_properties(layer_bridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${MGR_SPIRV_CROSS_ROOT}"
    OUTPUT_VARIABLE _cross_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(mgr_metal4 PRIVATE MGR_SPIRV_CROSS_REV="${_cross_revision}")
target_include_directories(mgr_metal4 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(mgr_metal4 SYSTEM PRIVATE "${MGR_METAL_CPP_ROOT}")
target_include_directories(mgr_metal4 SYSTEM PRIVATE $<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>)
# The SDK runtime already owns SDL in plugin processes. Only the standalone
# smoke executable links SDL directly, avoiding duplicate Objective-C classes.
target_link_libraries(mgr_metal4 PRIVATE spirv-cross-msl Threads::Threads
    "-framework Metal" "-framework QuartzCore" "-framework Foundation")
target_compile_options(mgr_metal4 PRIVATE -fblocks -Wall -Wextra -Werror=return-type)
# Do not inherit the SDK runtime's older deployment target or host-native ISA.
set_target_properties(mgr_metal4 PROPERTIES OSX_ARCHITECTURES arm64)

find_program(MGR_GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(_smoke_spirv)
foreach(stage vert frag)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/smoke.${stage}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${MGR_GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
            "${CMAKE_CURRENT_SOURCE_DIR}/smoke.${stage}" -o "${output}"
        DEPENDS "smoke.${stage}" VERBATIM)
    list(APPEND _smoke_spirv "${output}")
endforeach()
add_executable(mgr_metal4_smoke smoke.cpp ${_smoke_spirv})
target_link_libraries(mgr_metal4_smoke PRIVATE mgr_metal4 SDL3::SDL3)
target_compile_definitions(mgr_metal4_smoke PRIVATE MGR_SMOKE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(mgr_metal4_smoke PROPERTIES OSX_ARCHITECTURES arm64)

option(MGR_METAL4_XENOS_SHADER_PROBE "Build the shared Xenos shader translation probe" OFF)
option(MGR_METAL4_XENOS_INTEGRATION "Build the runtime-facing native Xenos plugin" ${MGR_BUILD_GAME})
if(MGR_METAL4_XENOS_SHADER_PROBE OR MGR_METAL4_XENOS_INTEGRATION)
    find_package(rexglue REQUIRED CONFIG)
    set(_xenos "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk")
    set(_shader "${_xenos}/src/graphics/pipeline/shader")
    add_library(mgr_xenos_shader STATIC
        ${_shader}/shader.cpp ${_shader}/translator.cpp ${_shader}/translator_disasm.cpp
        ${_shader}/spirv.cpp ${_shader}/spirv_builder.cpp ${_shader}/spirv_translator.cpp
        ${_shader}/spirv_translator_alu.cpp ${_shader}/spirv_translator_fetch.cpp
        ${_shader}/spirv_translator_memexport.cpp ${_shader}/spirv_translator_rb.cpp
        ${_xenos}/src/graphics/format/ucode.cpp
        ${_xenos}/src/graphics/flags.cpp ${_xenos}/src/graphics/util/draw.cpp
        ${_xenos}/src/graphics/registers.cpp
        ${_xenos}/src/graphics/pipeline/texture/util.cpp
        ${_xenos}/src/graphics/pipeline/texture/info_formats.cpp
        ${_xenos}/src/graphics/pipeline/render_target/psi_format.cpp)
    target_include_directories(mgr_xenos_shader BEFORE PUBLIC "${_xenos}/include"
        "${CMAKE_SOURCE_DIR}/vendor/glslang")
    target_link_libraries(mgr_xenos_shader PUBLIC rex::runtime rex::SPIRV rex::glslang rex::xxhash)
    target_compile_options(mgr_xenos_shader PRIVATE -Wno-deprecated-declarations)
    set_target_properties(mgr_xenos_shader PROPERTIES OSX_ARCHITECTURES arm64)
    if(MGR_METAL4_XENOS_SHADER_PROBE)
        add_executable(mgr_xenos_msl_probe xenos_shader_probe.cpp)
        target_link_libraries(mgr_xenos_msl_probe PRIVATE mgr_xenos_shader mgr_metal4)
        set_target_properties(mgr_xenos_msl_probe PROPERTIES OSX_ARCHITECTURES arm64)
    endif()
endif()
if(MGR_METAL4_XENOS_INTEGRATION)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_embedded_args)
    set(_runtime_spirv)
    foreach(shader ui.vert ui.frag present.vert)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${MGR_GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
                "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${output}"
            DEPENDS "${shader}" VERBATIM)
        list(APPEND _runtime_spirv "${output}")
    endforeach()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/runtime_shaders.h"
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/codegen/embed_spirv.py"
            "${CMAKE_CURRENT_BINARY_DIR}/runtime_shaders.h"
            "ui_vertex=${CMAKE_CURRENT_BINARY_DIR}/ui.vert.spv"
            "ui_fragment=${CMAKE_CURRENT_BINARY_DIR}/ui.frag.spv"
            "present_vertex=${CMAKE_CURRENT_BINARY_DIR}/present.vert.spv"
        DEPENDS ${_runtime_spirv} "${CMAKE_SOURCE_DIR}/tools/codegen/embed_spirv.py" VERBATIM)
    # Shared by the plugin and integration checks so SDK cvars and the shared
    # PM4 frontend are instantiated once per process.
    # Guest format conversion traverses complete textures/resolves. Keep these
    # loops optimized in Debug too, with symbols and runtime checks intact.
    set_source_files_properties(guest_renderer.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
    add_library(mgr_metal4_runtime SHARED runtime.cpp command_processor.cpp guest_renderer.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/runtime_shaders.h"
        ${_xenos}/src/graphics/graphics_system.cpp ${_xenos}/src/graphics/command_processor.cpp
        ${_xenos}/src/graphics/register_file.cpp
        ${CMAKE_SOURCE_DIR}/gpu/mgr_cmd_stats.cpp)
    target_include_directories(mgr_metal4_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(mgr_metal4_runtime PUBLIC mgr_metal4 mgr_xenos_shader)
    set_target_properties(mgr_metal4_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON OSX_ARCHITECTURES arm64
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_library(rexgpu-xenos-metal4 SHARED plugin_main.cpp)
    target_link_libraries(rexgpu-xenos-metal4 PRIVATE mgr_metal4_runtime)
    set_target_properties(rexgpu-xenos-metal4 PROPERTIES
        OUTPUT_NAME_DEBUG rexgpu-xenos-metal4d OUTPUT_NAME_RELWITHDEBINFO rexgpu-xenos-metal4rd
        OUTPUT_NAME_RELEASE rexgpu-xenos-metal4
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON OSX_ARCHITECTURES arm64)
    add_executable(mgr_metal4_integration_checks integration_checks.cpp)
    target_include_directories(mgr_metal4_integration_checks PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(mgr_metal4_integration_checks PRIVATE mgr_metal4_runtime)
    set_target_properties(mgr_metal4_integration_checks PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" OSX_ARCHITECTURES arm64)
    add_dependencies(mgr_metal4_integration_checks rexgpu-xenos-metal4)
endif()
