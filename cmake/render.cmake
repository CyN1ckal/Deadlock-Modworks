# Default model tools GUI, Vulkan renderer and their tests.

option(SR_BUILD_MODEL_TOOLS "Build the default model viewing, comparison and repair GUI" ON)
if(NOT SR_BUILD_MODEL_TOOLS)
    message(STATUS "Building the repair-only GUI (SR_BUILD_MODEL_TOOLS=OFF).")
    return()
endif()
find_package(Vulkan REQUIRED COMPONENTS glslc)

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

add_library(sr_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(sr_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(sr_imgui PUBLIC Vulkan::Vulkan)

set(SR_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SR_SHADER_DIR})
set(SR_SHADER_OUTPUTS)
foreach(shader mesh.vert mesh.frag line.vert line.frag)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader})
    set(output ${SR_SHADER_DIR}/${shader}.spv.inc)
    add_custom_command(
        OUTPUT ${output}
        # Vulkan 1.2 SPIR-V: under 1.3 rules, discard becomes demote-to-helper, which needs an extra device feature.
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.2 -O -mfmt=num -o ${output} ${source}
        DEPENDS ${source}
        COMMENT "Compiling ${shader}"
        VERBATIM
    )
    list(APPEND SR_SHADER_OUTPUTS ${output})
endforeach()

add_library(sr_render STATIC
    src/render/preview_scene.cpp
    src/render/model_renderer.cpp
    src/render/model_comparison.cpp
    src/render/model_versions.cpp
    src/render/preview_materials.cpp
    src/render/skeleton_pose.cpp
    ${SR_SHADER_OUTPUTS}
)
target_include_directories(sr_render PUBLIC src/render PRIVATE ${SR_SHADER_DIR})
target_link_libraries(sr_render PUBLIC skeleton_repair_core Vulkan::Vulkan)

add_library(sr_model_tools STATIC
    src/preview/main.cpp
    src/preview/model_viewport.cpp
    src/preview/preview_panel.cpp
    src/preview/comparison_view.cpp
    src/preview/repair_panel.cpp
    src/preview/library_panel.cpp
    src/preview/background.cpp
)
target_include_directories(sr_model_tools PUBLIC src/preview)
target_link_libraries(sr_model_tools PUBLIC sr_render sr_imgui shell32 comdlg32)
target_link_libraries(DeadlockSkeletonRepair PRIVATE sr_model_tools)
target_compile_definitions(DeadlockSkeletonRepair PRIVATE SR_MODEL_TOOLS)
add_executable(ModelPreview src/preview/standalone.cpp)
target_link_libraries(ModelPreview PRIVATE sr_model_tools)

add_executable(render_tests
    tests/preview_scene_tests.cpp
    tests/render_tests.cpp
    tests/comparison_tests.cpp
    tests/skeleton_pose_tests.cpp
)
target_link_libraries(render_tests PRIVATE sr_render GTest::gtest_main)

if(MSVC)
    foreach(target sr_render sr_model_tools ModelPreview render_tests)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

gtest_discover_tests(render_tests DISCOVERY_MODE PRE_TEST)
