#include "preview_panel.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace skeleton_repair {

PreviewPanel::PreviewPanel(ModelRenderer& renderer) : renderer_(renderer) {
    options_.VisibleMeshes = visible_;
}

PreviewPanel::~PreviewPanel() {
    if (texture_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture_);
    }
}

void PreviewPanel::Load(const ModelGeometry& geometry, std::string title) {
    scene_ = BuildPreviewScene(geometry);
    renderer_.SetScene(scene_);
    title_ = std::move(title);
    error_.clear();
    lod_ = 0;
    visible_ = DefaultVisibility(scene_, lod_);
    Frame();
}

void PreviewPanel::ShowError(std::string title, std::string message) {
    renderer_.ClearScene();
    scene_ = {};
    visible_.clear();
    title_ = std::move(title);
    error_ = std::move(message);
}

void PreviewPanel::Frame() {
    const float yaw = camera_.Yaw;
    const float pitch = camera_.Pitch;
    camera_ = OrbitCamera::Framing(scene_.Min, scene_.Max);
    camera_.Yaw = yaw;
    camera_.Pitch = pitch;
}

void PreviewPanel::RefreshTexture() {
    if (texture_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture_);
    }
    texture_ = ImGui_ImplVulkan_AddTexture(renderer_.Sampler(), renderer_.View(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PreviewPanel::Draw() {
    record_ = false;
    ImGui::TextUnformatted(title_.empty() ? "No model loaded." : title_.c_str());
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (scene_.Meshes.empty() && scene_.BoneCount == 0) {
        return;
    }

    // Controls.
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderInt("LOD", &lod_, 0, std::max(scene_.LodCount - 1, 0))) {
        visible_ = DefaultVisibility(scene_, lod_);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Skeleton", &options_.ShowSkeleton);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &options_.ShowGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Material colors", &options_.ColorByMaterial);
    ImGui::SameLine();
    if (ImGui::Button("Frame")) {
        Frame();
    }

    uint32_t triangles = 0;
    uint32_t vertices = 0;
    for (size_t i = 0; i < scene_.Meshes.size(); ++i) {
        if (visible_[i] != 0) {
            triangles += scene_.Meshes[i].TriangleCount;
            vertices += scene_.Meshes[i].VertexCount;
        }
    }
    ImGui::Text("%u triangles, %u vertices, %zu bones", triangles, vertices, scene_.BoneCount);

    if (!scene_.Meshes.empty() && ImGui::CollapsingHeader("Meshes")) {
        for (size_t i = 0; i < scene_.Meshes.size(); ++i) {
            bool shown = visible_[i] != 0;
            char label[256];
            std::snprintf(label, sizeof(label), "%s (%u tris)##mesh%zu", scene_.Meshes[i].Name.c_str(), scene_.Meshes[i].TriangleCount, i);
            if (ImGui::Checkbox(label, &shown)) {
                visible_[i] = shown ? 1 : 0;
            }
        }
    }

    // Viewport.
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(available.x, 32.0f), std::max(available.y, 32.0f)};
    if (renderer_.Resize(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y)) || texture_ == VK_NULL_HANDLE) {
        RefreshTexture();
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("viewport", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            camera_.Orbit(io.MouseDelta.x, io.MouseDelta.y);
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            camera_.Pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
        }
    }
    if (ImGui::IsItemHovered()) {
        if (io.MouseWheel != 0) {
            camera_.Zoom(io.MouseWheel);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            Frame();
        }
        ImGui::SetItemTooltip("Left drag: orbit   Right/middle drag: pan   Wheel: zoom   F: frame");
    }
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture_)), origin,
        ImVec2(origin.x + size.x, origin.y + size.y));
    record_ = true;
}

void PreviewPanel::Record(VkCommandBuffer command_buffer) {
    if (!record_) {
        return;
    }
    options_.VisibleMeshes = visible_;
    renderer_.Record(command_buffer, camera_, options_);
}

}  // namespace skeleton_repair
