cmake_minimum_required(VERSION 3.22.1)
project(sotc_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sotc_android SHARED
    jni_bridge.cpp
    iso_source.cpp
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

# Keep PS2Recomp isolated until its Android host layer and GS backend are adapted.
option(SOTC_LINK_PS2_RUNTIME "Build and link the upstream PS2Recomp runtime" OFF)

if(SOTC_LINK_PS2_RUNTIME)
    set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_RUNTIME ON CACHE BOOL "" FORCE)
    set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
    set(PS2X_ENABLE_DEBUG_UI OFF CACHE BOOL "" FORCE)
    set(PS2X_ENABLE_FFMPEG OFF CACHE BOOL "" FORCE)
    set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

    add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/../../../../../external/PS2Recomp
        ${CMAKE_CURRENT_BINARY_DIR}/ps2recomp
        EXCLUDE_FROM_ALL
    )
    target_link_libraries(sotc_android PRIVATE ps2_runtime)
    target_compile_definitions(sotc_android PRIVATE SOTC_HAS_PS2_RUNTIME=1)
endif()
