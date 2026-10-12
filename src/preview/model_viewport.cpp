#include "model_viewport.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdint>

namespace skeleton_repair {

bool DrawViewSettings(ViewSettings& settings, int lod_count, bool& frame) {
    bool lod_changed = false;
    if (lod_count > 1) {
        ImGui::SetNextItemWidth(90);
        lod_changed = ImGui::SliderInt("LOD", &settings.Lod, 0, lod_count - 1);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Skeleton", &settings.ShowSkeleton);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &settings.ShowGrid);
    ImGui::SameLine();
    static const char* const kModes[] = {"Clay", "Material colors", "Bone colors"};
    int mode = static_cast<int>(settings.Coloring);
    ImGui::SetNextItemWidth(130);
    if (ImGui::Combo("##coloring", &mode, kModes, IM_ARRAYSIZE(kModes))) {
        settings.Coloring = static_cast<ColorMode>(mode);
    }
    ImGui::SetItemTooltip("Bone colors: each vertex takes the color of the bone that moves it most.");
    ImGui::SameLine();
    frame = ImGui::Button("Frame");
    return lod_changed;
}

ModelViewport::ModelViewport(const VulkanDevice& device) : renderer_(device) {}

ModelViewport::~ModelViewport() {
    if (texture_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture_);
    }
}

void ModelViewport::SetModel(const ModelGeometry* geometry, int lod) {
    if (geometry == nullptr) {
        renderer_.ClearScene();
        scene_ = {};
        visible_.clear();
        return;
    }
    scene_ = BuildPreviewScene(*geometry);
    renderer_.SetScene(scene_);
    SetLod(lod);
}

void ModelViewport::SetSkeleton(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors) {
    scene_.Skeleton = SkeletonLines(bones, colors);
    renderer_.SetSkeleton(scene_.Skeleton);
}

void ModelViewport::SetLod(int lod) {
    visible_ = DefaultVisibility(scene_, std::clamp(lod, 0, std::max(scene_.LodCount - 1, 0)));
}

bool ModelViewport::Draw(const char* id, ImVec2 size, OrbitCamera& camera) {
    drawn_ = false;
    size.x = std::max(size.x, 32.0f);
    size.y = std::max(size.y, 32.0f);
    if (renderer_.Resize(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)) || texture_ == VK_NULL_HANDLE) {
        if (texture_ != VK_NULL_HANDLE) {
            ImGui_ImplVulkan_RemoveTexture(texture_);
        }
        texture_ = ImGui_ImplVulkan_AddTexture(renderer_.Sampler(), renderer_.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const ImGuiIO& io = ImGui::GetIO();
    bool moved = false;
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            camera.Orbit(io.MouseDelta.x, io.MouseDelta.y);
            moved = true;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            camera.Pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
            moved = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) {
            camera.Zoom(io.MouseWheel);
            moved = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            camera = OrbitCamera::Framing(scene_.Min, scene_.Max);
            moved = true;
        }
        ImGui::SetItemTooltip("Left drag: orbit   Right/middle drag: pan   Wheel: zoom   F: frame");
    }
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture_)), origin,
        ImVec2(origin.x + size.x, origin.y + size.y));
    drawn_ = true;
    return moved;
}

void ModelViewport::Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const ViewSettings& settings) {
    if (!drawn_) {
        return;
    }
    RenderOptions options;
    options.VisibleMeshes = visible_;
    options.ShowSkeleton = settings.ShowSkeleton;
    options.ShowGrid = settings.ShowGrid;
    options.Coloring = settings.Coloring;
    renderer_.Record(command_buffer, camera, options);
    drawn_ = false;
}

}  // namespace skeleton_repair
