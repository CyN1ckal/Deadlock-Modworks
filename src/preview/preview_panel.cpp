#include "preview_panel.hpp"

#include <imgui.h>

#include <cstdio>
#include <utility>

namespace skeleton_repair {

PreviewPanel::PreviewPanel(const VulkanDevice& device) : viewport_(device) {}

void PreviewPanel::Load(const PreparedModel& model, std::string title, ResourceReader read,
    const std::vector<GeometryBone>* animation_skeleton, const std::vector<std::string>& clips) {
    settings_.Lod = 0;
    viewport_.SetModel(&model, settings_.Lod);
    viewport_.SetAnimationSkeleton(animation_skeleton);
    read_ = std::move(read);
    player_.SetClips(viewport_.CanAnimate() && read_ ? clips : std::vector<std::string>{});
    animation_warning_.clear();
    title_ = std::move(title);
    error_.clear();
    const OrbitCamera framed = OrbitCamera::Framing(viewport_.Scene().Min, viewport_.Scene().Max);
    camera_.Target = framed.Target;
    camera_.Distance = framed.Distance;
}

void PreviewPanel::PlayClip(const std::string& path) {
    if (read_) {
        player_.Select(path, read_);
    }
}

void PreviewPanel::ShowError(std::string title, std::string message) {
    viewport_.SetModel(static_cast<const PreparedModel*>(nullptr), 0);
    player_.SetClips({});
    read_ = {};
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

    if (viewport_.CanAnimate() && read_) {
        player_.Draw(read_);
        player_.Advance(ImGui::GetIO().DeltaTime);
        animation_warning_ = viewport_.Animate(player_.Clip(), player_.Time());
        if (!animation_warning_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "%s", animation_warning_.c_str());
        }
    } else {
        ImGui::TextDisabled("No animation skeleton, so clips cannot play.");
    }

    viewport_.Draw("viewport", ImGui::GetContentRegionAvail(), camera_);
}

void PreviewPanel::Record(VkCommandBuffer command_buffer) {
    viewport_.Record(command_buffer, camera_, settings_);
}

}  // namespace skeleton_repair
