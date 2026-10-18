include_guard(GLOBAL)

if(NOT ANDROID)
    message(FATAL_ERROR "Configure Kagami with Android NDK 30.0.16248370 and android.toolchain.cmake; host builds are not supported")
endif()

set(KAGAMI_REQUIRED_NDK "30.0.16248370")

if(ANDROID)
    file(READ "${CMAKE_ANDROID_NDK}/source.properties" kagami_ndk_properties)
    string(REGEX MATCH "Pkg.Revision = ([^\r\n]+)" kagami_ndk_revision "${kagami_ndk_properties}")
    if(NOT CMAKE_MATCH_1 STREQUAL KAGAMI_REQUIRED_NDK)
        message(FATAL_ERROR "Kagami requires Android NDK ${KAGAMI_REQUIRED_NDK}")
    endif()
    # NDK Release defaults contain -O2. Optimization belongs to the target;
    # retain other toolchain flags, including unwind and security settings.
    foreach(language C CXX)
        foreach(config "" _RELEASE _RELWITHDEBINFO _MINSIZEREL)
            string(REGEX REPLACE "(^| )-O([0123sgz]|fast)( |$)" " "
                CMAKE_${language}_FLAGS${config} "${CMAKE_${language}_FLAGS${config}}")
        endforeach()
    endforeach()
endif()

function(kagami_production target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE -Wall -Wextra
        "$<$<COMPILE_LANGUAGE:CXX>:-Wpessimizing-move;-Wredundant-move;-Werror=pessimizing-move;-Werror=redundant-move>")
    if(NOT ANDROID)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:NDEBUG>")
    target_compile_options(${target} PRIVATE -Os -flto=thin -ffunction-sections -fdata-sections -fvisibility=hidden
        "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fvisibility-inlines-hidden>")
    target_link_options(${target} PRIVATE -Os -flto=thin -fuse-ld=lld -Wl,--gc-sections -Wl,--icf=safe)
endfunction()
