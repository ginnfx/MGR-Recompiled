# Native D3D-hook renderer (src/native, selected at run time by mgr_renderer).
# Built from re:Blue's plume renderer (vendor/reblue, BSD-3) adapted to MGR,
# plume itself (MIT), and the MGR_NATIVE_PIPELINE shader cache produced by
# build-tools/xenosrecomp-native (see patches/xenosrecomp-mgr.patch).

option(MGR_NATIVE_RENDERER "Build the native D3D-hook renderer" ON)
if(NOT MGR_NATIVE_RENDERER)
    return()
endif()

enable_language(C OBJC OBJCXX)  # zstd decoder (C), plume_apple.mm
set(MGR_REBLUE_DIR ${CMAKE_SOURCE_DIR}/vendor/reblue)
set(MGR_REBLUE_THIRDPARTY ${MGR_REBLUE_DIR}/thirdparty)
set(MGR_XENOSRECOMP_DIR ${MGR_REBLUE_THIRDPARTY}/XenosRecomp/XenosRecomp)
set(MGR_NATIVE_GEN_DIR ${CMAKE_BINARY_DIR}/native-gen)
set(MGR_NATIVE_SHADER_CACHE ${CMAKE_SOURCE_DIR}/generated/shaders-native/shader_cache.cpp)
if(NOT EXISTS ${MGR_NATIVE_SHADER_CACHE})
    message(FATAL_ERROR "Missing ${MGR_NATIVE_SHADER_CACHE}: run build-tools/xenosrecomp-native "
                        "(cmake -DCMAKE_CXX_FLAGS='-DREBLUE_RECOMP -DMGR_NATIVE_PIPELINE') over private/shaders")
endif()

add_subdirectory(${MGR_REBLUE_THIRDPARTY}/plume ${CMAKE_BINARY_DIR}/plume EXCLUDE_FROM_ALL)

# Host utility shaders (copy, gamma/present, MSAA resolve) compiled by dxc into
# headers, as re:Blue's cmake/shaders.cmake does.
set(MGR_DXC_BIN ${MGR_REBLUE_THIRDPARTY}/XenosRecomp/thirdparty/dxc-bin)
set(MGR_DXC "DYLD_LIBRARY_PATH=${MGR_DXC_BIN}/lib/arm64" "${MGR_DXC_BIN}/bin/arm64/dxc-macos")
set(MGR_NATIVE_HLSL_DIR ${CMAKE_SOURCE_DIR}/src/native/gpu/shaders/hlsl)
set(MGR_NATIVE_HOST_SHADER_HEADERS "")
function(mgr_native_host_shader STEM PROFILE)
    file(GLOB hlsl_includes "${MGR_NATIVE_HLSL_DIR}/*.hlsli")
    set(format_args -spirv -fvk-use-dx-layout)
    if(PROFILE MATCHES "^vs")
        list(APPEND format_args -fvk-invert-y)
    endif()
    set(out "${MGR_NATIVE_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.spirv.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${MGR_DXC}
                -T ${PROFILE} -HV 2021 -all-resources-bound
                -Wno-ignored-attributes ${format_args}
                -I "${MGR_REBLUE_DIR}"
                -Fh "${out}" "${MGR_NATIVE_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_spirv ${ARGN}
        DEPENDS "${MGR_NATIVE_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
                "${MGR_XENOSRECOMP_DIR}/shader_common.h"
        COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, spirv)"
        VERBATIM)
    list(APPEND MGR_NATIVE_HOST_SHADER_HEADERS "${out}")
    set(MGR_NATIVE_HOST_SHADER_HEADERS "${MGR_NATIVE_HOST_SHADER_HEADERS}" PARENT_SCOPE)
endfunction()
foreach(shader IN ITEMS copy_vs imgui_vs)
    mgr_native_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS copy_color_ps copy_depth_ps gamma_correction_ps imgui_ps pfx_occlusion_count_ps
        resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
        resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
    mgr_native_host_shader(${shader} ps_6_0)
endforeach()

file(GLOB_RECURSE MGR_NATIVE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/native/*.cpp ${CMAKE_SOURCE_DIR}/src/native/*.h)
# Objective-C++ (the MetalFX upscaler), compiled with ARC.
file(GLOB_RECURSE MGR_NATIVE_OBJC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/native/*.mm)
set_source_files_properties(${MGR_NATIVE_OBJC_SOURCES} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
list(APPEND MGR_NATIVE_SOURCES ${MGR_NATIVE_OBJC_SOURCES})
# OBJECT, not STATIC: a static archive drops the REX_HOOK override symbols
# because nothing references them by name.
add_library(mgr_native OBJECT ${MGR_NATIVE_SOURCES} ${MGR_NATIVE_HOST_SHADER_HEADERS}
    ${MGR_NATIVE_SHADER_CACHE}
    ${MGR_REBLUE_THIRDPARTY}/zstd/zstddeclib.c
    ${MGR_REBLUE_THIRDPARTY}/XenosRecomp/thirdparty/smol-v/source/smolv.cpp)
target_include_directories(mgr_native PRIVATE
    ${CMAKE_SOURCE_DIR}/src/native
    ${CMAKE_SOURCE_DIR}/src
    ${MGR_NATIVE_GEN_DIR}
    ${MGR_XENOSRECOMP_DIR}
    ${MGR_REBLUE_THIRDPARTY}/zstd
    ${MGR_REBLUE_THIRDPARTY}/XenosRecomp/thirdparty/smol-v/source
    ${CMAKE_SOURCE_DIR}/generated/default)
target_compile_definitions(mgr_native PRIVATE REBLUE_MVK MGR_NATIVE_RENDERER=1)
target_compile_options(mgr_native PRIVATE -Wno-deprecated-declarations)
set_source_files_properties(
    ${MGR_REBLUE_THIRDPARTY}/zstd/zstddeclib.c
    ${MGR_REBLUE_THIRDPARTY}/XenosRecomp/thirdparty/smol-v/source/smolv.cpp
    ${MGR_NATIVE_SHADER_CACHE}
    PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(mgr_native PRIVATE plume rex::runtime rex::xxhash)
mgr_apply_compiler_options(mgr_native)

target_sources(mgr PRIVATE $<TARGET_OBJECTS:mgr_native>)
target_link_libraries(mgr PRIVATE plume rex::xxhash "-framework Cocoa" "-framework QuartzCore"
    "-framework Metal" "-framework MetalFX" "-framework Foundation" "-framework IOKit"
    "-framework IOSurface")
target_compile_definitions(mgr PRIVATE MGR_NATIVE_RENDERER=1)
