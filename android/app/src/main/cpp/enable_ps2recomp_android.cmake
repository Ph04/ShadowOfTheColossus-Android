# Android-only adaptations for the pinned PS2Recomp submodule.
set(PS2RECOMP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../external/PS2Recomp")
get_filename_component(PS2RECOMP_ROOT "${PS2RECOMP_ROOT}" ABSOLUTE)

if(NOT EXISTS "${PS2RECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Pinned PS2Recomp submodule is missing: ${PS2RECOMP_ROOT}")
endif()

set(_root_cmake "${PS2RECOMP_ROOT}/CMakeLists.txt")
file(READ "${_root_cmake}" _root_text)
if(NOT _root_text MATCHES "PS2X_BUILD_COMPILER_TOOLS_ON_ANDROID")
    string(REPLACE
        "option(PS2X_BUILD_STUDIO \"Build ps2xStudio\" ON)\n\nif(ANDROID)"
        "option(PS2X_BUILD_STUDIO \"Build ps2xStudio\" ON)\noption(PS2X_BUILD_COMPILER_TOOLS_ON_ANDROID \"Build the PS2Recomp library for an Android in-process host\" OFF)\n\nif(ANDROID AND NOT PS2X_BUILD_COMPILER_TOOLS_ON_ANDROID)"
        _root_patched "${_root_text}")
    if(_root_patched STREQUAL _root_text)
        message(FATAL_ERROR "Could not apply the Android compiler-tools option to PS2Recomp CMakeLists.txt")
    endif()
    file(WRITE "${_root_cmake}" "${_root_patched}")
endif()

# Thread-safe progress snapshots from the recompiler's worker threads.
set(_reporter_h "${PS2RECOMP_ROOT}/ps2xRecomp/include/ps2recomp/recompiler_reporter.h")
file(READ "${_reporter_h}" _reporter_h_text)
if(NOT _reporter_h_text MATCHES "snapshotCounters\\(\\) const")
    string(REPLACE
        "const Counters &counters() const;"
        "const Counters &counters() const;\n        Counters snapshotCounters() const;"
        _reporter_h_patched "${_reporter_h_text}")
    if(_reporter_h_patched STREQUAL _reporter_h_text)
        message(FATAL_ERROR "Could not add RecompilerReporter::snapshotCounters declaration")
    endif()
    file(WRITE "${_reporter_h}" "${_reporter_h_patched}")
endif()

set(_reporter_cpp "${PS2RECOMP_ROOT}/ps2xRecomp/src/lib/recompiler_reporter.cpp")
file(READ "${_reporter_cpp}" _reporter_cpp_text)
if(NOT _reporter_cpp_text MATCHES "RecompilerReporter::snapshotCounters")
    string(REPLACE
        "const RecompilerReporter::Counters &RecompilerReporter::counters() const\n    {\n        return m_counters;\n    }"
        "const RecompilerReporter::Counters &RecompilerReporter::counters() const\n    {\n        return m_counters;\n    }\n\n    RecompilerReporter::Counters RecompilerReporter::snapshotCounters() const\n    {\n        std::lock_guard<std::mutex> lock(m_mutex);\n        return m_counters;\n    }"
        _reporter_cpp_patched "${_reporter_cpp_text}")
    if(_reporter_cpp_patched STREQUAL _reporter_cpp_text)
        message(FATAL_ERROR "Could not add RecompilerReporter::snapshotCounters implementation")
    endif()
    file(WRITE "${_reporter_cpp}" "${_reporter_cpp_patched}")
endif()

set(_recompiler_h "${PS2RECOMP_ROOT}/ps2xRecomp/include/ps2recomp/ps2_recompiler.h")
file(READ "${_recompiler_h}" _recompiler_h_text)
if(NOT _recompiler_h_text MATCHES "reportCountersSnapshot\\(\\) const")
    string(REPLACE
        "const RecompilerReporter::Counters &reportCounters() const { return m_reporter.counters(); }"
        "const RecompilerReporter::Counters &reportCounters() const { return m_reporter.counters(); }\n        RecompilerReporter::Counters reportCountersSnapshot() const { return m_reporter.snapshotCounters(); }"
        _recompiler_h_patched "${_recompiler_h_text}")
    if(_recompiler_h_patched STREQUAL _recompiler_h_text)
        message(FATAL_ERROR "Could not expose the lock-protected PS2Recompiler progress snapshot")
    endif()
    file(WRITE "${_recompiler_h}" "${_recompiler_h_patched}")
endif()

set(PS2X_BUILD_COMPILER_TOOLS_ON_ANDROID ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_RECOMP ON CACHE BOOL "" FORCE)
set(PS2X_BUILD_RUNTIME OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory("${PS2RECOMP_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/ps2recomp" EXCLUDE_FROM_ALL)

if(TARGET ps2_recomp)
    set_property(TARGET ps2_recomp PROPERTY EXCLUDE_FROM_ALL TRUE)
endif()
if(NOT TARGET ps2_recomp_lib)
    message(FATAL_ERROR "PS2Recomp did not define ps2_recomp_lib for the Android target")
endif()

target_link_libraries(sotc_android PRIVATE ps2_recomp_lib)
target_compile_definitions(sotc_android PRIVATE SOTC_HAS_PS2_RECOMPILER=1)
