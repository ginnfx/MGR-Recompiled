# mgr - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(mgr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MGR_SOURCES
    src/main.cpp
    src/mgr_perf.cpp
    src/mgr_kernel_fixes.cpp
    src/thirdparty/mgrr_perf/perf_logger.cpp
)

if(WIN32)
    add_executable(mgr WIN32 ${MGR_SOURCES})
else()
    add_executable(mgr ${MGR_SOURCES})
endif()

add_subdirectory(gpu)
rexglue_setup_target(mgr GPU_PLUGINS xenos)

# Source-built runtime with MGR fixes (runtime/CMakeLists.txt, scripts/build-runtime.sh).
# Same exports and install name as the SDK binary; staged after (over) the SDK copy.
set(MGR_RUNTIME_LIB "${CMAKE_SOURCE_DIR}/build-tools/rexruntime/librexruntimerd.dylib"
    CACHE FILEPATH "Source-built librexruntimerd.dylib to stage instead of the SDK binary")
if(EXISTS "${MGR_RUNTIME_LIB}")
    message(STATUS "mgr: staging source-built runtime ${MGR_RUNTIME_LIB}")
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

target_include_directories(mgr PRIVATE src/thirdparty/mgrr_perf)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE MGR_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(mgr PRIVATE MGR_BUILD_ID="${MGR_GIT_REV}")
