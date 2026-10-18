cmake_minimum_required(VERSION 3.22)

project(Kagami VERSION 0.1.0 LANGUAGES CXX)

option(KAGAMI_STRIP_ANDROID_BINARY "Strip Android binaries during release builds" ON)
option(KAGAMI_PACKAGE_WEBUI "Include the bundled WebUI in module packages" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(cmake/Production.cmake)
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
    src/core/flat_json.cpp
    src/core/runtime.cpp
    src/core/json_value.cpp
    src/core/lkm.cpp
    src/mount/backend.cpp
    src/mount/module_scan.cpp
    src/mount/module_settings.cpp
    src/mount/mount_fs.cpp
    src/mount/module_runtime.cpp
    src/mount/kasumi.cpp
    src/mount/kasumi_client.cpp
    src/mount/magic_mount.cpp
    src/mount/overlayfs.cpp
    src/mount/storage.cpp
)

add_executable(kagamid ${KAGAMI_SOURCES})
kagami_production(kagamid)
include(cmake/ClangTidy.cmake)
kagami_enable_clang_tidy(kagamid)
if(ANDROID)
    target_link_libraries(kagamid PRIVATE z)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(kagamid PRIVATE ZLIB::ZLIB)
endif()
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

option(KAGAMI_BUILD_NATIVE_TESTS "Build focused native runtime fixture tests" OFF)
if(KAGAMI_BUILD_NATIVE_TESTS)
    enable_testing()
    add_executable(module_runtime_test
        tests/native/module_runtime_test.cpp
        src/mount/module_runtime.cpp
        src/core/runtime.cpp
        src/core/json_value.cpp
        src/core/config.cpp
        src/core/flat_json.cpp
        src/core/log.cpp
    )
    target_include_directories(module_runtime_test PRIVATE include src)
    target_compile_options(module_runtime_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME module_runtime COMMAND module_runtime_test)
    add_test(NAME module_runtime_mounts COMMAND module_runtime_test --mounts)
    set_tests_properties(module_runtime_mounts PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(magic_runtime_test
        tests/native/magic_runtime_test.cpp
        src/mount/magic_mount.cpp
        src/mount/mount_fs.cpp
        src/mount/module_runtime.cpp
        src/core/runtime.cpp
        src/core/json_value.cpp
        src/core/config.cpp
        src/core/flat_json.cpp
        src/core/log.cpp
    )
    target_include_directories(magic_runtime_test PRIVATE include src)
    target_compile_options(magic_runtime_test PRIVATE -Wall -Wextra -Wpedantic)
    if(ANDROID)
        target_link_libraries(magic_runtime_test PRIVATE z)
    else()
        target_link_libraries(magic_runtime_test PRIVATE ZLIB::ZLIB)
    endif()
    add_test(NAME magic_runtime COMMAND magic_runtime_test)
    set_tests_properties(magic_runtime PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(kasumi_rules_test
        tests/native/kasumi_rules_test.cpp
        src/mount/kasumi_client.cpp
    )
    target_include_directories(kasumi_rules_test PRIVATE include)
    target_compile_options(kasumi_rules_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME kasumi_rules COMMAND kasumi_rules_test)
    add_executable(kasumi_runtime_test
        tests/native/kasumi_runtime_test.cpp
        src/mount/kasumi.cpp
        src/mount/module_settings.cpp
        src/mount/mount_fs.cpp
        src/mount/module_runtime.cpp
        src/core/runtime.cpp
        src/core/json_value.cpp
        src/core/config.cpp
        src/core/flat_json.cpp
        src/core/log.cpp
    )
    target_include_directories(kasumi_runtime_test PRIVATE include src)
    target_compile_options(kasumi_runtime_test PRIVATE -Wall -Wextra -Wpedantic)
    if(ANDROID)
        target_link_libraries(kasumi_runtime_test PRIVATE z)
    else()
        target_link_libraries(kasumi_runtime_test PRIVATE ZLIB::ZLIB)
    endif()
    add_test(NAME kasumi_runtime COMMAND kasumi_runtime_test)
    add_executable(config_read_test
        tests/native/config_read_test.cpp
        src/core/config.cpp
        src/core/flat_json.cpp
        src/core/json_value.cpp
        src/core/runtime.cpp
        src/core/log.cpp
    )
    target_include_directories(config_read_test PRIVATE include src)
    target_compile_options(config_read_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME config_read COMMAND config_read_test)
endif()

set(LKMLOADER_STRIP_ANDROID_BINARY ${KAGAMI_STRIP_ANDROID_BINARY}
    CACHE BOOL "Strip the bundled lkmloader Android binary" FORCE)
add_subdirectory(third_party/lkmloader)
get_target_property(kagami_loader_options lkmloader COMPILE_OPTIONS)
list(REMOVE_ITEM kagami_loader_options -Oz)
set_property(TARGET lkmloader PROPERTY COMPILE_OPTIONS "${kagami_loader_options}")
kagami_production(lkmloader)

if(ANDROID)
    set_target_properties(kagamid PROPERTIES OUTPUT_NAME "kagamid-${ANDROID_ABI}")
    target_compile_options(kagamid PRIVATE
        -fPIE
    )
    target_link_options(kagamid PRIVATE
        -pie
    )
    if(KAGAMI_STRIP_ANDROID_BINARY)
        target_link_options(kagamid PRIVATE -Wl,--strip-all)
    endif()
endif()

# Keep readable sources separate from the self-contained distribution. Node
# is only required when building the WebUI or a package that includes it.
if(CMAKE_HOST_WIN32)
    find_program(KAGAMI_NPM_EXECUTABLE NAMES npm.cmd npm)
else()
    find_program(KAGAMI_NPM_EXECUTABLE NAMES npm)
endif()
if(NOT KAGAMI_NPM_EXECUTABLE)
    set(KAGAMI_NPM_EXECUTABLE npm)
endif()
set(KAGAMI_WEBUI_DIR "${CMAKE_SOURCE_DIR}/build/webui")
add_custom_target(webui
    COMMAND ${KAGAMI_NPM_EXECUTABLE} ci --include=dev --no-audit --no-fund
    COMMAND ${KAGAMI_NPM_EXECUTABLE} run bundle
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/webui
    COMMENT "Bundling the offline Kagami WebUI"
    VERBATIM
)

set(KAGAMI_OUT_DIR "${CMAKE_BINARY_DIR}/out")
set(KAGAMI_PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
set(KAGAMI_PACKAGE_ZIP "${KAGAMI_OUT_DIR}/${KAGAMI_MODULE_ID}-${KAGAMI_MODULE_VERSION}.zip")

set(KAGAMI_WEBUI_PACKAGE_COMMAND)
set(KAGAMI_WEBUI_PACKAGE_DEPENDENCY)
if(KAGAMI_PACKAGE_WEBUI)
    set(KAGAMI_WEBUI_PACKAGE_COMMAND
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${KAGAMI_WEBUI_DIR} ${KAGAMI_PACKAGE_DIR}/webroot)
    set(KAGAMI_WEBUI_PACKAGE_DEPENDENCY webui)
endif()

add_custom_target(package
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KAGAMI_OUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${KAGAMI_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/module ${KAGAMI_PACKAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${KAGAMI_PACKAGE_DIR}/webroot
    ${KAGAMI_WEBUI_PACKAGE_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kagamid> ${KAGAMI_PACKAGE_DIR}/kagamid
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lkmloader> ${KAGAMI_PACKAGE_DIR}/lkmloader
    COMMAND ${CMAKE_COMMAND} -E chdir ${KAGAMI_PACKAGE_DIR} ${CMAKE_COMMAND} -E tar cf ${KAGAMI_PACKAGE_ZIP} --format=zip .
    DEPENDS kagamid lkmloader ${KAGAMI_WEBUI_PACKAGE_DEPENDENCY}
    COMMENT "Creating ${KAGAMI_PACKAGE_ZIP}"
    VERBATIM
)
