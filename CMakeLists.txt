# mgr - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mgr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/MgrCompilerOptions.cmake)
include(generated/rexglue.cmake)

# Sources
set(MGR_SOURCES
    src/main.cpp
    src/mgr_perf.cpp
    src/mgr_kernel_fixes.cpp
    src/mgr_timing.cpp
    src/mgr_input_prompts.cpp
    src/mgr_key_labels.cpp
    src/mgr_button_atlas.cpp
    src/mgr_icon_render_mac.cpp
    src/mgr_prompt_device.cpp
    src/mgr_settings.cpp
    src/mgr_qol.cpp
    src/mgr_postfx.cpp
    src/mgr_lod.cpp
    src/mgr_d3d_trace.cpp
    src/mgr_options_dialog.cpp
    src/mgr_game_options.cpp
    src/mgr_native_options.cpp
    src/mgr_options_dump.cpp
    src/mgr_options_pictures.cpp
    src/mgr_picture_codec.cpp
    src/mgr_options_sprite.cpp
    src/mgr_options_trace.cpp
    src/mgr_menu_fonts.cpp
    src/mgr_test_script.cpp
    src/mgr_test_unlock.cpp
    src/mgr_test_window_mac.cpp
    src/thirdparty/mgrr_perf/perf_logger.cpp
)

if(WIN32)
    add_executable(mgr WIN32 ${MGR_SOURCES})
else()
    add_executable(mgr ${MGR_SOURCES})
endif()

add_subdirectory(gpu)
# Original font data is extracted locally, never tracked in Git.
if(EXISTS "${CMAKE_SOURCE_DIR}/private/game/media/data000.cpk")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/mgr_ui_font.bin"
        COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
            $<TARGET_FILE:Python3::Interpreter> "${CMAKE_SOURCE_DIR}/tools/assets/prepare_ui_font.py"
            "${CMAKE_SOURCE_DIR}/private/game/media/data000.cpk"
            "${CMAKE_BINARY_DIR}/mgr_ui_font.bin"
        DEPENDS tools/assets/prepare_ui_font.py tools/assets/mgr_cpk.py
            "${CMAKE_SOURCE_DIR}/private/game/media/data000.cpk"
        VERBATIM)
    add_custom_target(mgr_menu_assets DEPENDS "${CMAKE_BINARY_DIR}/mgr_ui_font.bin")
    add_dependencies(mgr mgr_menu_assets)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/private/mgr_native_shaders.xrnb")
    add_custom_target(mgr_native_shader_asset
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/private/mgr_native_shaders.xrnb"
            "${CMAKE_BINARY_DIR}/mgr_native_shaders.xrnb")
    add_dependencies(mgr mgr_native_shader_asset)
endif()
rexglue_setup_target(mgr GPU_PLUGINS xenos)
foreach(_target mgr mgr_recomp rexgpu-xenos)
    mgr_apply_compiler_options(${_target})
endforeach()
# rexgpu-mgrnull is staged next to mgr by its LIBRARY_OUTPUT_DIRECTORY.
add_dependencies(mgr rexgpu-mgrnull)
include(cmake/native_renderer.cmake)
# Guest addresses may alias through different scalar/vector access types.
# Higher optimization must not assume ordinary C++ typed-object alias rules.
# PPC integer arithmetic wraps; generated addi/addic use signed C++ intermediates.
# Preserve that behavior under stronger LLVM range/overflow analysis.
target_compile_options(mgr_recomp PRIVATE -fno-strict-aliasing -fwrapv)
target_compile_options(rexgpu-xenos PRIVATE -fno-strict-aliasing)

# Source-built runtime with MGR fixes (runtime/CMakeLists.txt, scripts/build-runtime.sh).
# Same exports and install name as the SDK binary; staged after (over) the SDK copy.
set(MGR_RUNTIME_LIB "${CMAKE_SOURCE_DIR}/build-tools/rexruntime/librexruntimerd.dylib"
    CACHE FILEPATH "Source-built librexruntimerd.dylib to stage instead of the SDK binary")
if(EXISTS "${MGR_RUNTIME_LIB}")
    message(STATUS "mgr: staging source-built runtime ${MGR_RUNTIME_LIB}")
    # A runtime-only rebuild must also update the launched copy when mgr does
    # not need relinking (POST_BUILD alone missed this case).
    add_custom_target(mgr_runtime_stage
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MGR_RUNTIME_LIB}" "$<TARGET_FILE_DIR:mgr>/librexruntimerd.dylib")
    add_dependencies(mgr mgr_runtime_stage)
    add_custom_command(TARGET mgr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${MGR_RUNTIME_LIB}" "$<TARGET_FILE_DIR:mgr>/"
        COMMENT "Staging source-built librexruntimerd.dylib")
else()
    message(WARNING "mgr: ${MGR_RUNTIME_LIB} not built; using the SDK runtime "
                    "(missing presenter/wait fixes). Run scripts/build-runtime.sh")
endif()

# mgr_register.cpp is one ~85k-statement function that takes Apple Clang 30+
# minutes even at -O0; swap it for an equivalent table loop.
get_target_property(_recomp_sources mgr_recomp SOURCES)
list(FILTER _recomp_sources EXCLUDE REGEX "mgr_register\\.cpp$")
set_target_properties(mgr_recomp PROPERTIES SOURCES "${_recomp_sources}")
target_sources(mgr_recomp PRIVATE src/mgr_register_fast.cpp)

# Audit and transform only call-free integer leaf bodies in the build tree.
# Guest RAM remains volatile; original generated files and symbol names stay intact.
option(MGR_LOCALIZE_LEAF_REGISTERS "Cache registers in audited call-free guest bodies" ON)
if(MGR_LOCALIZE_LEAF_REGISTERS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_target_property(_leaf_sources mgr_recomp SOURCES)
    set(_optimized_sources "")
    foreach(_source IN LISTS _leaf_sources)
        if(_source MATCHES "mgr_recomp\\.[0-9]+\\.cpp$")
            get_filename_component(_source_abs "${_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
            get_filename_component(_source_name "${_source}" NAME)
            set(_output "${CMAKE_BINARY_DIR}/recomp-optimized/${_source_name}")
            set(_report "${_output}.json")
            add_custom_command(OUTPUT "${_output}" "${_report}"
                COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/localize_leaf_registers.py"
                    --input "${_source_abs}" --output "${_output}" --report "${_report}"
                DEPENDS "${_source_abs}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/localize_leaf_registers.py"
                VERBATIM)
            get_source_file_property(_source_options "${_source}" COMPILE_OPTIONS)
            if(_source_options)
                set_source_files_properties("${_output}" PROPERTIES COMPILE_OPTIONS "${_source_options}")
            endif()
            list(APPEND _optimized_sources "${_output}")
        else()
            list(APPEND _optimized_sources "${_source}")
        endif()
    endforeach()
    set_target_properties(mgr_recomp PROPERTIES SOURCES "${_optimized_sources}")
endif()


target_include_directories(mgr PRIVATE src/thirdparty/mgrr_perf)
target_link_libraries(mgr PRIVATE "-framework CoreText" "-framework CoreGraphics"
                      "-framework CoreFoundation")
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE MGR_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(mgr PRIVATE MGR_BUILD_ID="${MGR_GIT_REV}")

# First-run install of the player's game files (packaged app only).
enable_language(OBJCXX)
target_sources(mgr PRIVATE src/mgr_game_install.cpp src/mgr_disc_source.cpp src/mgr_dlc.cpp
    src/mgr_dlc_package.cpp src/mgr_setup_ui.cpp src/mgr_setup_wizard.cpp
    src/mgr_setup_mac.mm)
set_source_files_properties(src/mgr_setup_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(mgr PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers"
                      "-framework ImageIO")
# Setup screen fonts (OFL) and key art. private/setup_background.jpg is a
# local-only override; the shipped default is the app icon art.
set(MGR_SETUP_BACKGROUND "${CMAKE_SOURCE_DIR}/cmake/macos/AppIcon-source.jpg")
if(EXISTS "${CMAKE_SOURCE_DIR}/private/setup_background.jpg")
    set(MGR_SETUP_BACKGROUND "${CMAKE_SOURCE_DIR}/private/setup_background.jpg")
endif()
add_custom_target(mgr_setup_assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/setup"
        "${CMAKE_BINARY_DIR}/setup"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MGR_SETUP_BACKGROUND}"
        "${CMAKE_BINARY_DIR}/setup/background.jpg"
    VERBATIM)
add_dependencies(mgr mgr_setup_assets)
if(NOT MGR_NATIVE_RENDERER)  # cmake/native_renderer.cmake links it otherwise
    target_link_libraries(mgr PRIVATE rex::xxhash)
endif()

# Explicit target for the opt-in timing hook checks; excluded from normal builds.
add_executable(mgr_timing_checks EXCLUDE_FROM_ALL
    tests/mgr_timing_checks.cpp src/mgr_timing.cpp)
target_include_directories(mgr_timing_checks PRIVATE src)
target_link_libraries(mgr_timing_checks PRIVATE rex::runtime)

# Keyboard prompt atlas checks: mgr_prompt_checks <core.wtb> [out.ppm]
add_executable(mgr_prompt_checks EXCLUDE_FROM_ALL
    tests/mgr_prompt_checks.cpp src/mgr_key_labels.cpp src/mgr_button_atlas.cpp
    src/mgr_icon_render_mac.cpp)
target_include_directories(mgr_prompt_checks PRIVATE src)
target_link_libraries(mgr_prompt_checks PRIVATE rex::runtime "-framework CoreText"
                      "-framework CoreGraphics" "-framework CoreFoundation")

# Native XDK math reconstruction (MGR_NATIVE_MATH=0 restores the guest bodies).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
target_sources(mgr PRIVATE src/mgr_native_math.cpp)
set_source_files_properties(src/mgr_native_math.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/native-math-oracle.cpp"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/codegen/extract_oracle.py"
        --generated "${CMAKE_SOURCE_DIR}/generated/default"
        --output "${CMAKE_BINARY_DIR}/native-math-oracle.cpp"
    DEPENDS mgr_codegen ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
        tools/codegen/extract_oracle.py
    VERBATIM)
add_executable(mgr_native_math_checks EXCLUDE_FROM_ALL tests/mgr_native_math_checks.cpp
    src/mgr_native_math.cpp "${CMAKE_BINARY_DIR}/native-math-oracle.cpp")
target_include_directories(mgr_native_math_checks PRIVATE src generated/default)
target_link_libraries(mgr_native_math_checks PRIVATE rex::runtime)
mgr_apply_compiler_options(mgr_native_math_checks)

# D3D-library call census for native renderer bring-up (run with MGR_D3D_CENSUS=1).
option(MGR_D3D_CENSUS "Wrap every XDK D3D function to count calls per frame" OFF)
if(MGR_D3D_CENSUS)
    target_sources(mgr PRIVATE src/mgr_d3d_census.cpp)
    target_compile_definitions(mgr PRIVATE MGR_D3D_CENSUS=1)
endif()

# Native guest memcpy (MGR_NATIVE_MEMORY=0 restores the guest body).
target_sources(mgr PRIVATE src/mgr_native_memory.cpp)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/native-memory-oracle.cpp"
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/codegen/extract_oracle.py"
        --memory-only --generated "${CMAKE_SOURCE_DIR}/generated/default"
        --output "${CMAKE_BINARY_DIR}/native-memory-oracle.cpp"
    DEPENDS mgr_codegen ${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
        tools/codegen/extract_oracle.py VERBATIM)
add_executable(mgr_native_memory_checks EXCLUDE_FROM_ALL tests/mgr_native_memory_checks.cpp
    src/mgr_native_memory.cpp "${CMAKE_BINARY_DIR}/native-memory-oracle.cpp")
target_include_directories(mgr_native_memory_checks PRIVATE src generated/default)
target_link_libraries(mgr_native_memory_checks PRIVATE rex::runtime)
mgr_apply_compiler_options(mgr_native_memory_checks)

# Resolve arena unit test; header-only SDK code from the patched vendor tree.
add_executable(mgr_resolve_arena_checks EXCLUDE_FROM_ALL tests/resolve_arena_test.cpp)
target_include_directories(mgr_resolve_arena_checks PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_compile_options(mgr_resolve_arena_checks PRIVATE -UNDEBUG)
add_executable(mgr_scale_ratio_checks EXCLUDE_FROM_ALL tests/scale_ratio_test.cpp)
target_include_directories(mgr_scale_ratio_checks PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_compile_options(mgr_scale_ratio_checks PRIVATE -UNDEBUG)
add_executable(mgr_msaa_twin_checks EXCLUDE_FROM_ALL tests/msaa_twin_test.cpp)
target_include_directories(mgr_msaa_twin_checks PRIVATE
    "${CMAKE_SOURCE_DIR}/src/native"
    "${CMAKE_SOURCE_DIR}/vendor/reblue/thirdparty/XenosRecomp/XenosRecomp"
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_compile_options(mgr_msaa_twin_checks PRIVATE -UNDEBUG)
add_executable(mgr_scaled_texture_address_checks EXCLUDE_FROM_ALL
    tests/scaled_texture_address_test.cpp)
target_include_directories(mgr_scaled_texture_address_checks PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_compile_options(mgr_scaled_texture_address_checks PRIVATE -UNDEBUG)
add_executable(mgr_scaled_viewport_checks EXCLUDE_FROM_ALL tests/scaled_viewport_test.cpp)
target_include_directories(mgr_scaled_viewport_checks PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_link_libraries(mgr_scaled_viewport_checks PRIVATE rex::runtime)
target_compile_options(mgr_scaled_viewport_checks PRIVATE -UNDEBUG)
add_executable(mgr_ot_key_checks EXCLUDE_FROM_ALL tests/ot_key_test.cpp)
target_include_directories(mgr_ot_key_checks PRIVATE src)
target_compile_options(mgr_ot_key_checks PRIVATE -UNDEBUG)
add_executable(mgr_picture_codec_checks EXCLUDE_FROM_ALL tests/picture_codec_test.cpp
    src/mgr_picture_codec.cpp src/mgr_options_pictures.cpp)
target_include_directories(mgr_picture_codec_checks PRIVATE src)
target_compile_options(mgr_picture_codec_checks PRIVATE -UNDEBUG)
add_executable(mgr_lod_bias_checks EXCLUDE_FROM_ALL tests/lod_bias_test.cpp)
target_include_directories(mgr_lod_bias_checks PRIVATE src)
target_compile_options(mgr_lod_bias_checks PRIVATE -UNDEBUG)
add_executable(mgr_upscale_math_checks EXCLUDE_FROM_ALL tests/upscale_math_test.cpp)
target_include_directories(mgr_upscale_math_checks PRIVATE src
    "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include")
target_compile_options(mgr_upscale_math_checks PRIVATE -UNDEBUG)
add_executable(mgr_bundle_paths_checks EXCLUDE_FROM_ALL tests/bundle_paths_test.cpp)
target_include_directories(mgr_bundle_paths_checks PRIVATE src)
target_compile_options(mgr_bundle_paths_checks PRIVATE -UNDEBUG)
add_executable(mgr_game_install_checks EXCLUDE_FROM_ALL tests/game_install_test.cpp
    src/mgr_game_install.cpp)
target_include_directories(mgr_game_install_checks PRIVATE src)
target_link_libraries(mgr_game_install_checks PRIVATE rex::xxhash)
target_compile_options(mgr_game_install_checks PRIVATE -UNDEBUG)
add_executable(mgr_dlc_package_checks EXCLUDE_FROM_ALL tests/dlc_package_test.cpp
    src/mgr_dlc_package.cpp)
target_include_directories(mgr_dlc_package_checks PRIVATE src)
target_compile_options(mgr_dlc_package_checks PRIVATE -UNDEBUG)

# GPU check of the resolution-scaled resolve and texture load SPIR-V on MoltenVK
# (Homebrew vulkan-headers and molten-vk); ctest reports it skipped without a
# Vulkan device.
find_path(MGR_VULKAN_INCLUDE_DIR vulkan/vulkan.h PATHS /opt/homebrew/include)
if(MGR_VULKAN_INCLUDE_DIR)
  add_executable(mgr_rational_shader_gpu_checks EXCLUDE_FROM_ALL tests/rational_shader_gpu_test.cpp)
  target_include_directories(mgr_rational_shader_gpu_checks PRIVATE
      "${CMAKE_SOURCE_DIR}/vendor/rexglue-sdk/include" "${MGR_VULKAN_INCLUDE_DIR}")
  target_link_libraries(mgr_rational_shader_gpu_checks PRIVATE rex::runtime)
  target_compile_options(mgr_rational_shader_gpu_checks PRIVATE -UNDEBUG)
endif()

# `ninja -C build mgr_tests && ctest --test-dir build` runs every host-only check.
enable_testing()
set(MGR_TEST_TARGETS mgr_msaa_twin_checks mgr_resolve_arena_checks mgr_scale_ratio_checks mgr_scaled_viewport_checks
    mgr_scaled_texture_address_checks
    mgr_ot_key_checks mgr_lod_bias_checks mgr_upscale_math_checks mgr_bundle_paths_checks mgr_game_install_checks
    mgr_dlc_package_checks
    mgr_timing_checks mgr_picture_codec_checks
    mgr_native_math_checks mgr_native_memory_checks)
if(TARGET mgr_rational_shader_gpu_checks)
  list(APPEND MGR_TEST_TARGETS mgr_rational_shader_gpu_checks)
endif()
add_custom_target(mgr_tests DEPENDS ${MGR_TEST_TARGETS})
add_test(NAME resolve_arena COMMAND mgr_resolve_arena_checks)
add_test(NAME scale_ratio COMMAND mgr_scale_ratio_checks)
add_test(NAME msaa_twin COMMAND mgr_msaa_twin_checks)
add_test(NAME scaled_viewport COMMAND mgr_scaled_viewport_checks)
add_test(NAME scaled_texture_address COMMAND mgr_scaled_texture_address_checks)
if(TARGET mgr_rational_shader_gpu_checks)
  add_test(NAME rational_shader_gpu COMMAND mgr_rational_shader_gpu_checks)
  set_tests_properties(rational_shader_gpu PROPERTIES SKIP_RETURN_CODE 77)
endif()
add_test(NAME ot_key COMMAND mgr_ot_key_checks)
add_test(NAME lod_bias COMMAND mgr_lod_bias_checks)
add_test(NAME picture_codec COMMAND mgr_picture_codec_checks)
add_test(NAME upscale_math COMMAND mgr_upscale_math_checks)
add_test(NAME bundle_paths COMMAND mgr_bundle_paths_checks)
add_test(NAME game_install COMMAND mgr_game_install_checks)
add_test(NAME dlc_package COMMAND mgr_dlc_package_checks)
foreach(_mode enabled disabled forced)
    add_test(NAME timing_${_mode} COMMAND mgr_timing_checks ${_mode})
endforeach()
add_test(NAME native_math COMMAND mgr_native_math_checks)
add_test(NAME native_memory COMMAND mgr_native_memory_checks)
