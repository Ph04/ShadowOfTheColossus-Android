cmake_minimum_required(VERSION 3.22.1)
project(sotc_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sotc_android SHARED
    jni_bridge.cpp
    iso_source.cpp
    iso9660.cpp
    sha256.cpp
    disc_inspector.cpp
    xff_linker.cpp
    elf_writer.cpp
    on_device_pipeline.cpp
    vulkan_renderer.cpp
)

target_compile_definitions(sotc_android PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR=1
)

target_compile_options(sotc_android PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_include_directories(sotc_android PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sotc_android PRIVATE
    android
    log
    vulkan
)

# Build the upstream recompiler as an in-process Android ARM64 library.
# The command-line executable target is excluded; JNI calls PS2Recompiler directly.
include(${CMAKE_CURRENT_LIST_DIR}/enable_ps2recomp_android.cmake)
