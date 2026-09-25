# Intel ISA-L erasure_code only (C + optional ARM NEON).
include_guard(GLOBAL)

set(VSTREAMER_ISAL_EC_DIR "${CMAKE_CURRENT_LIST_DIR}")

enable_language(C)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    enable_language(ASM)
endif()

function(vstreamer_add_isal_ec)
    if(TARGET isal_ec)
        return()
    endif()

    include(FetchContent)
    FetchContent_Declare(isa-l
        GIT_REPOSITORY https://github.com/intel/isa-l.git
        GIT_TAG v2.31.1
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(isa-l)
    if(NOT isa-l_POPULATED)
        FetchContent_Populate(isa-l)
    endif()

    set(ISAL_ROOT "${isa-l_SOURCE_DIR}")
    set(ISAL_EC_SOURCES "${ISAL_ROOT}/erasure_code/ec_base.c")
    set(ISAL_EC_NEON OFF)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(ISAL_EC_NEON ON)
        file(MAKE_DIRECTORY "${ISAL_ROOT}/erasure_code/include")
        file(COPY "${ISAL_ROOT}/include/aarch64_label.h"
             DESTINATION "${ISAL_ROOT}/erasure_code/include")
        list(APPEND ISAL_EC_SOURCES
            "${VSTREAMER_ISAL_EC_DIR}/isal_ec_neon.c"
            "${ISAL_ROOT}/erasure_code/aarch64/gf_vect_dot_prod_neon.S"
            "${ISAL_ROOT}/erasure_code/aarch64/gf_2vect_dot_prod_neon.S"
            "${ISAL_ROOT}/erasure_code/aarch64/gf_3vect_dot_prod_neon.S"
            "${ISAL_ROOT}/erasure_code/aarch64/gf_4vect_dot_prod_neon.S"
            "${ISAL_ROOT}/erasure_code/aarch64/gf_5vect_dot_prod_neon.S"
        )
    endif()

    add_library(isal_ec STATIC ${ISAL_EC_SOURCES})
    target_include_directories(isal_ec
        PUBLIC
            "${ISAL_ROOT}/include"
        PRIVATE
            "${ISAL_ROOT}"
            "${ISAL_ROOT}/include"
            "${ISAL_ROOT}/erasure_code"
    )
    target_compile_definitions(isal_ec PRIVATE NDEBUG)
    if(ISAL_EC_NEON)
        target_compile_definitions(isal_ec PUBLIC VSTREAMER_ISAL_NEON=1)
    endif()
endfunction()
