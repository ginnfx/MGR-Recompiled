# Slim source build of the ReXGlue runtime (librexruntimerd.dylib) from
# vendor/rexglue-sdk (bd833a2 = the shipped SDK binary's source) so runtime-side
# fixes can ship without the full SDK superbuild (FFmpeg/SDL3/MoltenVK sources).
# Reuses the vendored per-module CMakeLists unchanged; third-party deps come
# from the prebuilt SDK (tools/sdk/mac-arm64). Local fixes: see PROGRESS.md.
#
#   cmake -S runtime -B build-tools/rexruntime -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
#   ninja -C build-tools/rexruntime -j4 rexruntime
cmake_minimum_required(VERSION 3.25)
# Before project(): match the SDK binary (minos 13.3).
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3 CACHE STRING "")
project(mgr_rexruntime C CXX OBJC OBJCXX)

get_filename_component(MGR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(REXGLUE_ROOT "${MGR_ROOT}/vendor/rexglue-sdk")
include_directories("${MGR_ROOT}/runtime")
set(REXSDK_PREFIX "${MGR_ROOT}/tools/sdk/mac-arm64")
foreach(required thirdparty/imgui/imgui.cpp thirdparty/cli11/include
                 thirdparty/FFmpeg/libavcodec/avcodec.h thirdparty/libmspack/cabextract/mspack/lzx.h)
    if(NOT EXISTS "${REXGLUE_ROOT}/${required}")
        message(FATAL_ERROR "Missing ${required}: git -C vendor/rexglue-sdk submodule update "
                            "--init --depth 1 thirdparty/cli11 thirdparty/imgui thirdparty/FFmpeg "
                            "thirdparty/libmspack")
    endif()
endforeach()

# Vendored module CMakeLists reference these as the SDK superbuild's root.
set(PROJECT_SOURCE_DIR "${REXGLUE_ROOT}")
set(PROJECT_BINARY_DIR "${CMAKE_BINARY_DIR}/sdk")
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/include/rex")
file(COPY "${REXSDK_PREFIX}/include/rex/version.h" DESTINATION "${PROJECT_BINARY_DIR}/include/rex")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
# Retain the SDK memory/FP semantics. Let Apple Clang select its supported
# Apple ARM baseline instead of forcing generic ARMv8-A in the host runtime.
add_compile_options(-mcmodel=small -fno-strict-aliasing -ffp-model=strict -fno-char8_t
                    -Wno-deprecated-declarations)
add_compile_definitions(REX_PLATFORM_MAC=1 REXGLUE_BUILD_CONFIG="$<CONFIG>"
                        REXGLUE_ENABLE_PROFILING REXGLUE_ENABLE_PERF_COUNTERS)
set(REXGLUE_USE_VULKAN ON)
set(REXGLUE_ENABLE_TRACY ON)
set(REXGLUE_ENABLE_PERF_COUNTERS ON)

include("${REXGLUE_ROOT}/cmake/rex_pch.cmake")
function(rexglue_add_version_resource)
endfunction()

# Prebuilt third-party targets, under the names the vendored CMakeLists use.
# (Not find_package(rexglue): its imported rex::runtime clashes with ours.)
list(PREPEND CMAKE_PREFIX_PATH "${REXSDK_PREFIX}")
find_package(Threads REQUIRED)
foreach(dep fmt spdlog utf8cpp SDL3 VulkanHeaders VulkanMemoryAllocator)
    find_package(${dep} REQUIRED CONFIG)
endforeach()
set(SDK_INC "${REXSDK_PREFIX}/include")
set(SDK_LIB "${REXSDK_PREFIX}/lib")
function(mgr_headers name)
    add_library(${name} INTERFACE)
    target_include_directories(${name} SYSTEM INTERFACE ${ARGN})
endfunction()
function(mgr_static name file)
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${SDK_LIB}/${file}"
                          INTERFACE_INCLUDE_DIRECTORIES "${SDK_INC}")
endfunction()
mgr_headers(simde "${SDK_INC}")
mgr_headers(tomlplusplus "${SDK_INC}")
mgr_headers(disruptorplus "${SDK_INC}/disruptorplus")
mgr_headers(renderdoc "${SDK_INC}/renderdoc")
mgr_headers(spirv-tools-headers "${SDK_INC}")
add_library(utf8cpp ALIAS utf8cpp::utf8cpp)
mgr_static(aes128 libaes128rd.a)
mgr_static(mspack libmspackrd.a)
set_property(TARGET mspack APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             "${REXGLUE_ROOT}/thirdparty/libmspack/cabextract/mspack")
mgr_static(o1heap libo1heaprd.a)
mgr_static(xxHash::xxhash libxxhashrd.a)
mgr_static(libavutil liblibavutilrd.a)
mgr_static(libavcodec liblibavcodecrd.a)
set_property(TARGET libavcodec APPEND PROPERTY INTERFACE_LINK_LIBRARIES libavutil)
set_property(TARGET libavcodec APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
             "${REXGLUE_ROOT}/thirdparty/FFmpeg")
add_library(TracyClient SHARED IMPORTED GLOBAL)
set_target_properties(TracyClient PROPERTIES
    IMPORTED_LOCATION "${SDK_LIB}/libTracyClientrd.dylib"
    INTERFACE_COMPILE_DEFINITIONS
        "TRACY_IMPORTS;TRACY_ENABLE;TRACY_ON_DEMAND;TRACY_DELAYED_INIT;TRACY_MANUAL_LIFETIME;TRACY_FIBERS"
    INTERFACE_INCLUDE_DIRECTORIES "${SDK_INC}/tracy"
    INTERFACE_LINK_LIBRARIES Threads::Threads)
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3::SDL3-static)
endif()

add_library(imgui OBJECT
    ${REXGLUE_ROOT}/thirdparty/imgui/imgui.cpp
    ${REXGLUE_ROOT}/thirdparty/imgui/imgui_demo.cpp
    ${REXGLUE_ROOT}/thirdparty/imgui/imgui_draw.cpp
    ${REXGLUE_ROOT}/thirdparty/imgui/imgui_tables.cpp
    ${REXGLUE_ROOT}/thirdparty/imgui/imgui_widgets.cpp)
add_library(imgui::imgui ALIAS imgui)
target_include_directories(imgui PUBLIC ${REXGLUE_ROOT}/thirdparty/imgui)
target_compile_options(imgui PRIVATE -w)

foreach(module core filesystem ui input audio system kernel)
    add_subdirectory("${REXGLUE_ROOT}/src/${module}" "${CMAKE_BINARY_DIR}/${module}")
endforeach()

# Drop-in for the SDK binary: same file name and install name.
set_target_properties(rexruntime PROPERTIES
    OUTPUT_NAME_RELWITHDEBINFO rexruntimerd
    INSTALL_NAME_DIR "@rpath"
    # Same LC_RPATHs as the SDK binary. Without this CMake embeds the absolute
    # tools/sdk lib dir, so @rpath/libTracyClientrd.dylib could resolve to a
    # second copy of Tracy next to the one the executable loads.
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path;@loader_path/../lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(rexruntime PRIVATE "-framework Cocoa" "-framework QuartzCore"
                      "-framework Metal" "-framework IOKit" "-framework AudioToolbox")

# Same optimization policy as the game/plugin, including every runtime object library.
include("${MGR_ROOT}/cmake/MgrCompilerOptions.cmake")
foreach(_target rexcore rexfilesystem rexui rexinput rexaudio rexruntime imgui)
    mgr_apply_compiler_options(${_target})
endforeach()
