#include "preview_panel.hpp"

#include <imgui.h>

#include <cstdio>
#include <utility>

namespace skeleton_repair {

PreviewPanel::PreviewPanel(const VulkanDevice& device) : viewport_(device) {}

void PreviewPanel::Load(const ModelGeometry& geometry, std::string title, const ResourceReader* read) {
    settings_.Lod = 0;
    viewport_.SetModel(&geometry, settings_.Lod, read);
    title_ = std::move(title);
    error_.clear();
    const OrbitCamera framed = OrbitCamera::Framing(viewport_.Scene().Min, viewport_.Scene().Max);
    camera_.Target = framed.Target;
    camera_.Distance = framed.Distance;
}

void PreviewPanel::ShowError(std::string title, std::string message) {
    viewport_.SetModel(nullptr, 0);
    title_ = std::move(title);
    error_ = std::move(message);
}

void PreviewPanel::Draw() {
    ImGui::TextUnformatted(title_.empty() ? "No model loaded." : title_.c_str());
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (viewport_.Empty()) {
        return;
    }

    const PreviewScene& scene = viewport_.Scene();
    bool frame = false;
    if (DrawViewSettings(settings_, scene.LodCount, frame)) {
        viewport_.SetLod(settings_.Lod);
    }
    if (frame) {
        const OrbitCamera framed = OrbitCamera::Framing(scene.Min, scene.Max);
        camera_.Target = framed.Target;
        camera_.Distance = framed.Distance;
    }

    std::vector<uint8_t>& visible = viewport_.Visible();
    uint32_t triangles = 0;
    uint32_t vertices = 0;
    for (size_t i = 0; i < scene.Meshes.size(); ++i) {
        if (visible[i] != 0) {
            triangles += scene.Meshes[i].TriangleCount;
            vertices += scene.Meshes[i].VertexCount;
        }
    }
    ImGui::Text("%u triangles, %u vertices, %zu bones, %zu/%zu materials textured", triangles, vertices, scene.BoneCount,
        viewport_.TexturedMaterials(), viewport_.MaterialCount());
    if (!viewport_.MaterialNotes().empty() && ImGui::CollapsingHeader("Material notes")) {
        for (const std::string& note : viewport_.MaterialNotes()) {
            ImGui::BulletText("%s", note.c_str());
        }
    }

    if (!scene.Meshes.empty() && ImGui::CollapsingHeader("Meshes")) {
        for (size_t i = 0; i < scene.Meshes.size(); ++i) {
            bool shown = visible[i] != 0;
            char label[256];
            std::snprintf(label, sizeof(label), "%s (%u tris)##mesh%zu", scene.Meshes[i].Name.c_str(), scene.Meshes[i].TriangleCount, i);
            if (ImGui::Checkbox(label, &shown)) {
                visible[i] = shown ? 1 : 0;
            }
        }
    }

    viewport_.Draw("viewport", ImGui::GetContentRegionAvail(), camera_);
}

void PreviewPanel::Record(VkCommandBuffer command_buffer) {
    viewport_.Record(command_buffer, camera_, settings_);
}

}  // namespace skeleton_repair
