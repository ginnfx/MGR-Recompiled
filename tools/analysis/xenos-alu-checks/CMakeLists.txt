# cmake -S tools/analysis/xenos-alu-checks -B build-tools/alu-checks -G Ninja
# cmake --build build-tools/alu-checks --target alu_source_checks --parallel 4
cmake_minimum_required(VERSION 3.20)
project(XenosAluChecks LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(MGR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(XR "${MGR_ROOT}/vendor/reblue/thirdparty/XenosRecomp")
set(XENOS_RECOMP_THIRDPARTY_ROOT "${XR}/thirdparty")
add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}" thirdparty)
add_executable(alu_source_checks "${MGR_ROOT}/tests/xenos_alu_source_checks.cpp"
    "${XR}/XenosRecomp/shader_recompiler.cpp")
target_link_libraries(alu_source_checks PRIVATE Microsoft::DirectXShaderCompiler
    xxHash::xxhash libzstd_static fmt::fmt)
target_include_directories(alu_source_checks PRIVATE "${XR}/XenosRecomp"
    "${XR}/thirdparty/smol-v/source")
target_precompile_headers(alu_source_checks PRIVATE "${XR}/XenosRecomp/pch.h")
target_compile_definitions(alu_source_checks PRIVATE REBLUE_RECOMP MGR_NATIVE_PIPELINE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(alu_source_checks PRIVATE -Wno-switch -Wno-unused-variable
        -Wno-null-arithmetic -fms-extensions -UNDEBUG)
endif()
enable_testing()
add_test(NAME alu_sources COMMAND alu_source_checks)
