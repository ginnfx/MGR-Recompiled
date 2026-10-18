include_guard(GLOBAL)

# Keep project compiler policy outside generated SDK files. Target options follow
# CMAKE_<LANG>_FLAGS, including older build directories configured with -O1.
set(MGR_OPTIMIZATION_LEVEL "2" CACHE STRING "Optimization for non-Debug project code (1, 2, 3, s, z)")
set_property(CACHE MGR_OPTIMIZATION_LEVEL PROPERTY STRINGS 1 2 3 s z)
if(NOT MGR_OPTIMIZATION_LEVEL MATCHES "^(1|2|3|s|z)$")
    message(FATAL_ERROR "Invalid MGR_OPTIMIZATION_LEVEL: ${MGR_OPTIMIZATION_LEVEL}")
endif()

option(MGR_OPTIMIZATION_REPORTS "Save LLVM optimization records beside object files" OFF)
option(MGR_THINLTO "Enable ThinLTO for non-Debug source targets" ON)
set(MGR_LTO_JOBS "2" CACHE STRING "Maximum Darwin ThinLTO backend threads")
if(NOT MGR_LTO_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "MGR_LTO_JOBS must be a positive integer")
endif()
set(_mgr_tune_default "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND NOT CMAKE_CROSSCOMPILING)
    set(_mgr_tune_default native)
endif()
set(MGR_CPU_TUNE "${_mgr_tune_default}" CACHE STRING
    "Optional CPU scheduling tune; native keeps the deployment instruction-set baseline")

set(MGR_PGO_MODE "OFF" CACHE STRING "LLVM PGO mode: OFF, GENERATE, USE")
set_property(CACHE MGR_PGO_MODE PROPERTY STRINGS OFF GENERATE USE)
string(TOUPPER "${MGR_PGO_MODE}" MGR_PGO_MODE)
if(NOT MGR_PGO_MODE MATCHES "^(OFF|GENERATE|USE)$")
    message(FATAL_ERROR "MGR_PGO_MODE must be OFF, GENERATE or USE")
endif()
set(MGR_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM .profdata for MGR_PGO_MODE=USE")
set(MGR_PGO_OUTPUT_DIR "${CMAKE_BINARY_DIR}/pgo-raw" CACHE PATH "Raw LLVM profile output directory")
if(MGR_PGO_MODE STREQUAL "USE" AND NOT EXISTS "${MGR_PGO_PROFILE}")
    message(FATAL_ERROR "MGR_PGO_MODE=USE requires an existing MGR_PGO_PROFILE .profdata")
endif()
if(MGR_PGO_MODE STREQUAL "GENERATE")
    file(MAKE_DIRECTORY "${MGR_PGO_OUTPUT_DIR}")
endif()

function(mgr_apply_compiler_options target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            "$<$<NOT:$<CONFIG:Debug>>:-O${MGR_OPTIMIZATION_LEVEL}>")
        get_target_property(_type ${target} TYPE)
        if(NOT _type STREQUAL "OBJECT_LIBRARY")
            target_link_options(${target} PRIVATE
                "$<$<NOT:$<CONFIG:Debug>>:-O${MGR_OPTIMIZATION_LEVEL}>")
        endif()
        if(MGR_OPTIMIZATION_REPORTS)
            target_compile_options(${target} PRIVATE -fsave-optimization-record)
        endif()
        if(MGR_CPU_TUNE)
            target_compile_options(${target} PRIVATE
                "$<$<NOT:$<CONFIG:Debug>>:-mtune=${MGR_CPU_TUNE}>")
        endif()
        if(MGR_PGO_MODE STREQUAL "GENERATE")
            # IR-based PGO is intended for optimization rather than source coverage.
            # Clang's default_%m.profraw separates dylibs and merges repeated runs.
            # Atomic updates preserve counters shared by guest/worker threads.
            set(_pgo_flag "-fprofile-generate=${MGR_PGO_OUTPUT_DIR}")
            target_compile_options(${target} PRIVATE "${_pgo_flag}" -fprofile-update=atomic)
            target_link_options(${target} PRIVATE "${_pgo_flag}")
        elseif(MGR_PGO_MODE STREQUAL "USE")
            target_compile_options(${target} PRIVATE "-fprofile-use=${MGR_PGO_PROFILE}"
                -Werror=profile-instr-out-of-date)
            target_link_options(${target} PRIVATE "-fprofile-use=${MGR_PGO_PROFILE}")
        endif()
        if(MGR_THINLTO)
            target_compile_options(${target} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:-flto=thin>")
            get_target_property(_type ${target} TYPE)
            if(NOT _type STREQUAL "OBJECT_LIBRARY")
                target_link_options(${target} PRIVATE
                    "$<$<NOT:$<CONFIG:Debug>>:-flto=thin>")
                if(APPLE)
                    # LLVM's ld64 backend option limits memory pressure. Cache is
                    # separate per final binary to avoid unrelated link eviction.
                    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lto-cache/${target}")
                    target_link_options(${target} PRIVATE
                        "$<$<NOT:$<CONFIG:Debug>>:LINKER:-mllvm,-threads=${MGR_LTO_JOBS}>"
                        "$<$<NOT:$<CONFIG:Debug>>:LINKER:-cache_path_lto,${CMAKE_BINARY_DIR}/lto-cache/${target}>")
                endif()
            endif()
        endif()
    else()
        message(FATAL_ERROR "MGR compiler options currently require Clang or AppleClang")
    endif()
endfunction()
