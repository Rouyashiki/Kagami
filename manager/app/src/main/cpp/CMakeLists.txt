cmake_minimum_required(VERSION 3.22.1)

project(kagami_manager_native LANGUAGES CXX)

add_library(kagami_client SHARED
    kagami_jni.cpp
)

target_compile_features(kagami_client PRIVATE cxx_std_17)
target_compile_options(kagami_client PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

target_link_libraries(kagami_client PRIVATE log)
