cmake_minimum_required(VERSION 3.22)

project(Kagami VERSION 0.1.0 LANGUAGES CXX)

option(KAGAMI_STRIP_ANDROID_BINARY "Strip Android binaries during release builds" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(READ "${CMAKE_SOURCE_DIR}/module/module.prop" KAGAMI_MODULE_PROP)
string(REGEX MATCH "id=([^\n]+)" _ "${KAGAMI_MODULE_PROP}")
set(KAGAMI_MODULE_ID "${CMAKE_MATCH_1}")
string(REGEX MATCH "version=([^\n]+)" _ "${KAGAMI_MODULE_PROP}")
set(KAGAMI_MODULE_VERSION "${CMAKE_MATCH_1}")

message(STATUS "Building ${KAGAMI_MODULE_ID} ${KAGAMI_MODULE_VERSION}")
if(ANDROID)
    message(STATUS "Target ABI: ${ANDROID_ABI}")
endif()

set(KAGAMI_SOURCES
    src/main.cpp
    src/core/command.cpp
    src/core/daemon.cpp
    src/core/log.cpp
    src/core/config.cpp
    src/core/runtime.cpp
    src/core/json_value.cpp
    src/core/lkm.cpp
    src/mount/backend.cpp
    src/mount/mount_fs.cpp
    src/mount/kasumi.cpp
    src/mount/kasumi_client.cpp
    src/mount/magic_mount.cpp
    src/mount/overlayfs.cpp
    src/mount/storage.cpp
)

add_executable(kagamid ${KAGAMI_SOURCES})
target_include_directories(kagamid PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(kagamid PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(kagamid PRIVATE
    KAGAMI_VERSION="${KAGAMI_MODULE_VERSION}"
)

set(LKMLOADER_STRIP_ANDROID_BINARY ${KAGAMI_STRIP_ANDROID_BINARY}
    CACHE BOOL "Strip the bundled lkmloader Android binary" FORCE)
add_subdirectory(third_party/lkmloader)

if(ANDROID)
    set_target_properties(kagamid PROPERTIES OUTPUT_NAME "kagamid-${ANDROID_ABI}")
    target_compile_options(kagamid PRIVATE
        -Oz
        -fPIE
        -ffunction-sections
        -fdata-sections
        -fvisibility=hidden
        -fvisibility-inlines-hidden
    )
    target_link_options(kagamid PRIVATE
        -pie
        -Wl,--gc-sections
    )
    if(KAGAMI_STRIP_ANDROID_BINARY)
        target_link_options(kagamid PRIVATE -Wl,--strip-all)
    endif()
endif()

# The WebUI is the static frontend committed in module/webroot (served by
# KernelSU WebUI X); there is no build step.

set(KAGAMI_OUT_DIR "${CMAKE_BINARY_DIR}/out")
set(KAGAMI_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(KAGAMI_PACKAGE_ZIP "${KAGAMI_OUT_DIR}/${KAGAMI_MODULE_ID}-${KAGAMI_MODULE_VERSION}.zip")

add_custom_target(package
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KAGAMI_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${KAGAMI_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/module ${KAGAMI_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kagamid> ${KAGAMI_PACKAGE_DIR}/kagamid
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lkmloader> ${KAGAMI_PACKAGE_DIR}/lkmloader
    COMMAND ${CMAKE_COMMAND} -E chdir ${KAGAMI_PACKAGE_DIR} ${CMAKE_COMMAND} -E tar cf ${KAGAMI_PACKAGE_ZIP} --format=zip .
    DEPENDS kagamid lkmloader
    COMMENT "Creating ${KAGAMI_PACKAGE_ZIP}"
    VERBATIM
)
