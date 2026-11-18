cmake_minimum_required(VERSION 3.24)

project(DeadlockModworks VERSION 0.2.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Deadlock Modworks builds on Windows x64.")
endif()

# Portable zip: static CRT (/MT, /MTd for Debug). vulkan-1.dll stays a driver import.
# CMP0091 is NEW here. Older subprojects ignore MSVC_RUNTIME_LIBRARY and keep the
# compiler default /MD, so the per-config flags carry /MT as well.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    foreach(lang C CXX)
        string(APPEND CMAKE_${lang}_FLAGS_RELEASE " /MT")
        string(APPEND CMAKE_${lang}_FLAGS_MINSIZEREL " /MT")
        string(APPEND CMAKE_${lang}_FLAGS_RELWITHDEBINFO " /MT")
        string(APPEND CMAKE_${lang}_FLAGS_DEBUG " /MTd")
    endforeach()
    set(SR_MSVC_RUNTIME "static")
else()
    set(SR_MSVC_RUNTIME "dynamic")
endif()

# One product identity for About, CLI --version, Windows VERSIONINFO, and the package file name.
set(SR_PRODUCT_NAME "Deadlock Modworks")
set(SR_PRODUCT_EXECUTABLE "DeadlockModworks")
set(SR_PACKAGE_ZIP "${SR_PRODUCT_EXECUTABLE}-${PROJECT_VERSION}-win-x64.zip")
find_package(Git QUIET)
set(SR_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE SR_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE SR_GIT_RESULT
    )
    if(NOT SR_GIT_RESULT EQUAL 0 OR SR_GIT_COMMIT STREQUAL "")
        set(SR_GIT_COMMIT "unknown")
    endif()
endif()
# rc.exe treats a backslash as an escape, so the icon path uses forward slashes.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/assets/branding/modworks.ico" SR_APP_ICON)
configure_file(cmake/product_version.hpp.in "${CMAKE_BINARY_DIR}/generated/product_version.hpp" @ONLY)
configure_file(cmake/app.rc.in "${CMAKE_BINARY_DIR}/generated/app.rc" @ONLY)
configure_file(cmake/app_icon.rc.in "${CMAKE_BINARY_DIR}/generated/app_icon.rc" @ONLY)
set_source_files_properties(
    "${CMAKE_BINARY_DIR}/generated/app.rc"
    "${CMAKE_BINARY_DIR}/generated/app_icon.rc"
    PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/branding/modworks.ico")
file(WRITE "${CMAKE_BINARY_DIR}/product_package_name.txt"
    "${SR_PRODUCT_EXECUTABLE}.exe\n${SR_PACKAGE_ZIP}\n${PROJECT_VERSION}\n${SR_MSVC_RUNTIME}\n")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(cmake/dependencies.cmake)

# Everything that is not drawing: file formats, the store, repairs, the library and its jobs, and the records the
# panels show. The console workflow, the repair-only window and every test build on it.
add_library(skeleton_repair_core STATIC
    src/core/strings.cpp
    src/core/skeleton.cpp
    src/core/vpk.cpp
    src/core/archive_cache.cpp
    src/core/archive_transaction.cpp
    src/core/kv3.cpp
    src/core/resource.cpp
    src/core/camera_attachment.cpp
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
    src/core/startup.cpp
    src/core/model_library.cpp
    src/core/character_view.cpp
    src/core/content_hash.cpp
    src/core/content_store.cpp
    src/core/mod_store.cpp
    src/core/store_jobs.cpp
    src/core/store_reader.cpp
    src/core/history_compare.cpp
    src/core/library_model.cpp
    src/core/model_versions.cpp
    src/core/version_builder.cpp
    src/core/archive_extract.cpp
    src/core/http_client.cpp
    src/core/gamebanana.cpp
    src/core/mod_state.cpp
    src/core/addons_deployer.cpp
    src/core/library_session.cpp
)
target_include_directories(skeleton_repair_core PUBLIC src/core "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(skeleton_repair_core PUBLIC bcrypt sr_lz4 libzstd_static sr_sqlite nlohmann_json::nlohmann_json meshoptimizer sr_archive winhttp advapi32 shell32 ole32 windowscodecs)

# Opens the model tools workspace by default; command-line options run the console workflow.
add_executable(DeadlockModworks
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/app.manifest
    "${CMAKE_BINARY_DIR}/generated/app.rc"
)
target_link_libraries(DeadlockModworks PRIVATE skeleton_repair_core shell32 ole32 comctl32)

add_executable(skeleton_repair_tests
    tests/skeleton_tests.cpp
    tests/vpk_tests.cpp
    tests/kv3_tests.cpp
    tests/resource_tests.cpp
    tests/camera_attachment_tests.cpp
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
    tests/startup_tests.cpp
    tests/mod_state_tests.cpp
    tests/addons_deployer_tests.cpp
    tests/gamebanana_tests.cpp
    tests/archive_extract_tests.cpp
    tests/archive_cache_tests.cpp
    tests/library_session_tests.cpp
)
target_link_libraries(skeleton_repair_tests PRIVATE skeleton_repair_core GTest::gtest_main)
target_compile_definitions(skeleton_repair_tests PRIVATE SR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

sr_project_warnings(skeleton_repair_core DeadlockModworks skeleton_repair_tests)

enable_testing()
include(GoogleTest)
gtest_discover_tests(skeleton_repair_tests DISCOVERY_MODE PRE_TEST)

# Fails when the include graph gets a cycle, a lower target includes a higher one, or a new
# stable-dependency violation appears. See scripts/architecture_diagram.py.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import networkx" RESULT_VARIABLE SR_NETWORKX_RESULT OUTPUT_QUIET ERROR_QUIET)
endif()
if(Python3_Interpreter_FOUND AND SR_NETWORKX_RESULT EQUAL 0)
    add_test(NAME dependency_levels COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/architecture_diagram.py --check)
else()
    message(STATUS "Python 3 with networkx not found: the dependency_levels test is not registered.")
endif()

include(cmake/render.cmake)
