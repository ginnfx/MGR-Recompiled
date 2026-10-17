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
)

if(WIN32)
    add_executable(mgr WIN32 ${MGR_SOURCES})
else()
    add_executable(mgr ${MGR_SOURCES})
endif()

rexglue_setup_target(mgr GPU_PLUGINS xenos)

# mgr_register.cpp is one ~85k-statement function that takes Apple Clang 30+
# minutes even at -O0; swap it for an equivalent table loop.
get_target_property(_recomp_sources mgr_recomp SOURCES)
list(FILTER _recomp_sources EXCLUDE REGEX "mgr_register\\.cpp$")
set_target_properties(mgr_recomp PROPERTIES SOURCES "${_recomp_sources}")
target_sources(mgr_recomp PRIVATE src/mgr_register_fast.cpp)
