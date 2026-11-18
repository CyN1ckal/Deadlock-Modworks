# Third-party libraries of the core library and the tests. The GUI's own (Dear ImGui, Tracy, Vulkan) are in render.cmake.

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# OFF keeps gtest on the same static CRT as the app. ON forces the shared CRT and the tests fail to link.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# lz4, zstd and SQLite are populated without their own build scripts and compiled as plain static libraries.
FetchContent_Declare(
    lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    zstd
    URL https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
foreach(dependency lz4 zstd sqlite3)
    FetchContent_GetProperties(${dependency})
    if(NOT ${dependency}_POPULATED)
        FetchContent_Populate(${dependency})
    endif()
endforeach()

add_library(sr_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
target_include_directories(sr_lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
# zstd rewrites /MD to /MT only when this is already set. Its cmake_minimum_required
# predates CMP0091, so the MSVC_RUNTIME_LIBRARY property alone does not reach it.
set(ZSTD_USE_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
add_subdirectory(${zstd_SOURCE_DIR}/build/cmake ${CMAKE_BINARY_DIR}/zstd-build EXCLUDE_FROM_ALL)

add_library(sr_sqlite STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sr_sqlite PUBLIC ${sqlite3_SOURCE_DIR})
# Stored mod files can be larger than SQLite's default 1 GB value limit.
target_compile_definitions(sr_sqlite PRIVATE
    SQLITE_MAX_LENGTH=2147483647
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
)
if(MSVC)
    target_compile_options(sr_sqlite PRIVATE /W0)
endif()

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    meshoptimizer
    URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.24.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(meshoptimizer)

# libarchive with zlib and xz, for downloaded mod archives.
include(${CMAKE_CURRENT_LIST_DIR}/archive_deps.cmake)

# The warning level every project target builds with.
function(sr_project_warnings)
    if(MSVC)
        foreach(target ${ARGN})
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        endforeach()
    endif()
endfunction()
