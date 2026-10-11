cmake_minimum_required(VERSION 3.24)

project(DeadlockSkeletonRepair VERSION 0.1.1 LANGUAGES CXX)

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

add_library(skeleton_repair_core STATIC
    src/core/strings.cpp
    src/core/skeleton.cpp
    src/core/vpk.cpp
    src/core/archive_transaction.cpp
)
target_include_directories(skeleton_repair_core PUBLIC src/core)
target_link_libraries(skeleton_repair_core PUBLIC bcrypt)

add_executable(skeleton_repair_tests
    tests/skeleton_tests.cpp
    tests/vpk_tests.cpp
)
target_link_libraries(skeleton_repair_tests PRIVATE skeleton_repair_core GTest::gtest_main bcrypt)

if(MSVC)
    target_compile_options(skeleton_repair_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(skeleton_repair_tests PRIVATE /W4 /permissive- /utf-8)
endif()

enable_testing()
include(GoogleTest)
gtest_discover_tests(skeleton_repair_tests DISCOVERY_MODE PRE_TEST)
