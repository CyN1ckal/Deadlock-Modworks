# Model preview: Vulkan renderer, ImGui preview tool and their tests.
# Needs the Vulkan SDK (headers, loader and glslc). Without it these targets are skipped.

find_package(Vulkan COMPONENTS glslc)
if(NOT Vulkan_FOUND OR NOT Vulkan_glslc_FOUND)
    message(STATUS "Vulkan SDK with glslc not found; skipping the model preview renderer.")
    return()
endif()

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
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num -o ${output} ${source}
        DEPENDS ${source}
        COMMENT "Compiling ${shader}"
        VERBATIM
    )
    list(APPEND SR_SHADER_OUTPUTS ${output})
endforeach()

add_library(sr_render STATIC
    src/render/preview_scene.cpp
    src/render/model_renderer.cpp
    ${SR_SHADER_OUTPUTS}
)
target_include_directories(sr_render PUBLIC src/render PRIVATE ${SR_SHADER_DIR})
target_link_libraries(sr_render PUBLIC skeleton_repair_core Vulkan::Vulkan)

add_executable(ModelPreview
    src/preview/main.cpp
    src/preview/preview_panel.cpp
)
target_link_libraries(ModelPreview PRIVATE sr_render sr_imgui shell32 comdlg32)

add_executable(render_tests
    tests/preview_scene_tests.cpp
    tests/render_tests.cpp
)
target_link_libraries(render_tests PRIVATE sr_render GTest::gtest_main)

if(MSVC)
    foreach(target sr_render ModelPreview render_tests)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

gtest_discover_tests(render_tests DISCOVERY_MODE PRE_TEST)
