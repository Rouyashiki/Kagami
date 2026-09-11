include_guard(GLOBAL)

option(KAGAMI_ENABLE_CLANG_TIDY "Check first-party native code with clang-tidy" ON)
set(KAGAMI_CLANG_TIDY_CONFIG "${CMAKE_CURRENT_LIST_DIR}/../.clang-tidy")

function(kagami_enable_clang_tidy target)
    if(NOT KAGAMI_ENABLE_CLANG_TIDY)
        return()
    endif()
    get_filename_component(compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(KAGAMI_CLANG_TIDY_PROGRAM NAMES clang-tidy
        HINTS "${compiler_dir}" REQUIRED)
    set_property(TARGET "${target}" PROPERTY CXX_CLANG_TIDY
        "${KAGAMI_CLANG_TIDY_PROGRAM};--config-file=${KAGAMI_CLANG_TIDY_CONFIG}")
endfunction()
