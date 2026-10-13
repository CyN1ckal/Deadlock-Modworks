cmake_minimum_required(VERSION 3.24)

project(DeadlockSkeletonRepair VERSION 0.1.1 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Deadlock Skeleton Repair builds on Windows x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(lz4)
if(NOT lz4_POPULATED)
    FetchContent_Populate(lz4)
endif()
FetchContent_GetProperties(zstd)
if(NOT zstd_POPULATED)
    FetchContent_Populate(zstd)
endif()

add_library(sr_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
target_include_directories(sr_lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(${zstd_SOURCE_DIR}/build/cmake ${CMAKE_BINARY_DIR}/zstd-build EXCLUDE_FROM_ALL)

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

FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(sqlite3)
if(NOT sqlite3_POPULATED)
    FetchContent_Populate(sqlite3)
endif()
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

add_library(skeleton_repair_core STATIC
    src/core/strings.cpp
    src/core/skeleton.cpp
    src/core/vpk.cpp
    src/core/archive_transaction.cpp
    src/core/kv3.cpp
    src/core/resource.cpp
    src/core/animation.cpp
    src/core/model.cpp
    src/core/updater.cpp
    src/core/report.cpp
    src/core/command_line.cpp
    src/core/model_geometry.cpp
    src/core/texture.cpp
    src/core/animation_clip.cpp
    src/core/clip_decoder.cpp
    src/core/game_folder.cpp
    src/core/model_library.cpp
    src/core/content_hash.cpp
    src/core/content_store.cpp
    src/core/mod_store.cpp
    src/core/model_versions.cpp
    src/core/version_builder.cpp
)
target_include_directories(skeleton_repair_core PUBLIC src/core)
target_link_libraries(skeleton_repair_core PUBLIC bcrypt sr_lz4 libzstd_static sr_sqlite nlohmann_json::nlohmann_json meshoptimizer advapi32 shell32 ole32 windowscodecs)

# Opens the model tools workspace by default; command-line options run the console workflow.
add_executable(DeadlockSkeletonRepair
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/app.manifest
)
target_link_libraries(DeadlockSkeletonRepair PRIVATE skeleton_repair_core shell32 ole32 comctl32)

add_executable(skeleton_repair_tests
    tests/skeleton_tests.cpp
    tests/vpk_tests.cpp
    tests/kv3_tests.cpp
    tests/resource_tests.cpp
    tests/animation_tests.cpp
    tests/model_tests.cpp
    tests/updater_tests.cpp
    tests/geometry_tests.cpp
    tests/texture_tests.cpp
    tests/animation_clip_tests.cpp
    tests/game_folder_tests.cpp
    tests/model_library_tests.cpp
    tests/store_tests.cpp
    tests/library_model_tests.cpp
    tests/store_reader_tests.cpp
    tests/history_compare_tests.cpp
    tests/character_view_tests.cpp
    tests/command_line_store_tests.cpp
    src/preview/library_model.cpp
    src/preview/character_view.cpp
    src/preview/store_reader.cpp
    src/preview/history_compare.cpp
)
target_include_directories(skeleton_repair_tests PRIVATE src/preview)
target_link_libraries(skeleton_repair_tests PRIVATE skeleton_repair_core GTest::gtest_main bcrypt)
target_compile_definitions(skeleton_repair_tests PRIVATE SR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(MSVC)
    target_compile_options(skeleton_repair_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(DeadlockSkeletonRepair PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(skeleton_repair_tests PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
include(GoogleTest)
gtest_discover_tests(skeleton_repair_tests DISCOVERY_MODE PRE_TEST)

include(cmake/render.cmake)
