#include "profile.hpp"
#include "preview_panel.hpp"

#include "ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
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

namespace {
void StatRow(const char* key, const std::string& value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", key);
    ImGui::TableNextColumn();
    ui::TextEllipsis(value);
}
}  // namespace

void PreviewPanel::DrawDetails() {
    const PreviewScene& scene = viewport_.Scene();
    std::vector<uint8_t>& visible = viewport_.Visible();
    uint32_t triangles = 0;
    uint32_t vertices = 0;
    size_t shown_meshes = 0;
    for (size_t i = 0; i < scene.Meshes.size(); ++i) {
        if (visible[i] != 0) {
            triangles += scene.Meshes[i].TriangleCount;
            vertices += scene.Meshes[i].VertexCount;
            ++shown_meshes;
        }
    }
    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)
        && ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        StatRow("Path", title_);
        if (ImGui::BeginPopupContextItem("path_menu")) {
            ui::CopyMenuItem("Copy path", title_);
            ImGui::EndPopup();
        }
        StatRow("Triangles", std::to_string(triangles));
        StatRow("Vertices", std::to_string(vertices));
        StatRow("Meshes", std::to_string(shown_meshes) + " of " + std::to_string(scene.Meshes.size()) + " shown");
        StatRow("Bones", std::to_string(scene.BoneCount));
        StatRow("LODs", std::to_string(std::max(scene.LodCount, 1)));
        StatRow("Textured", std::to_string(viewport_.TexturedMaterials()) + " of " + std::to_string(viewport_.MaterialCount()) + " materials");
        StatRow("Animation", viewport_.CanAnimate() && read_ ? "Clips play on the animation skeleton" : "No animation skeleton");
        ImGui::EndTable();
    }

    char header[64];
    std::snprintf(header, sizeof(header), "Meshes (%zu)###meshes", scene.Meshes.size());
    if (!scene.Meshes.empty() && ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ui::ToolButton("All", "Show every mesh of every LOD")) {
            std::fill(visible.begin(), visible.end(), uint8_t{1});
        }
        ImGui::SameLine();
        if (ui::ToolButton("None", "Hide every mesh")) {
            std::fill(visible.begin(), visible.end(), uint8_t{0});
        }
        ImGui::SameLine();
        if (ui::ToolButton("Reset", "Show the meshes of the current LOD")) {
            viewport_.SetLod(settings_.Lod);
        }
        if (ImGui::BeginTable("meshes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("##shown", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2, 1});
            for (size_t i = 0; i < scene.Meshes.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool shown = visible[i] != 0;
                if (ImGui::Checkbox("##shown", &shown)) {
                    visible[i] = shown ? 1 : 0;
                }
                ImGui::TableNextColumn();
                if (ImGui::Selectable(scene.Meshes[i].Name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    visible[i] = visible[i] != 0 ? 0 : 1;
                }
                if (ImGui::BeginPopupContextItem("mesh_menu")) {
                    if (ImGui::MenuItem("Show only this mesh")) {
                        std::fill(visible.begin(), visible.end(), uint8_t{0});
                        visible[i] = 1;
                    }
                    if (ImGui::MenuItem(visible[i] != 0 ? "Hide" : "Show")) {
                        visible[i] = visible[i] != 0 ? 0 : 1;
                    }
                    if (ImGui::MenuItem("Show all meshes")) {
                        std::fill(visible.begin(), visible.end(), uint8_t{1});
                    }
                    ImGui::Separator();
                    ui::CopyMenuItem("Copy name", scene.Meshes[i].Name);
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%u", scene.Meshes[i].TriangleCount);
                ImGui::PopID();
            }
            ImGui::PopStyleVar();
            ImGui::EndTable();
        }
    }

    const auto& notes = viewport_.MaterialNotes();
    std::snprintf(header, sizeof(header), "Material notes (%zu)###notes", notes.size());
    if (!notes.empty() && ImGui::CollapsingHeader(header)) {
        for (size_t i = 0; i < notes.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Bullet();
            ImGui::TextWrapped("%s", notes[i].c_str());
            if (ImGui::BeginPopupContextItem("note_menu")) {
                ui::CopyMenuItem("Copy note", notes[i]);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
}

void PreviewPanel::Draw() {
    ZoneScopedN("Preview");
    if (!error_.empty()) {
        if (!title_.empty()) {
            ui::TextEllipsis(title_);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kError);
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (viewport_.Empty()) {
        ImGui::TextDisabled("%s", title_.empty() ? "No model loaded." : title_.c_str());
        return;
    }

    const PreviewScene& scene = viewport_.Scene();
    const float scale = ImGui::GetFontSize() / 15.0f;
    bool frame = false;
    if (DrawViewSettings(settings_, scene.LodCount, frame)) {
        viewport_.SetLod(settings_.Lod);
    }
    if (viewport_.CanAnimate() && read_) {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < 520 * scale) {
            ImGui::NewLine();
        } else {
            ui::ToolSeparator();
        }
        player_.Draw(read_);
        player_.Advance(ImGui::GetIO().DeltaTime);
        animation_warning_ = viewport_.Animate(player_.Clip(), player_.Time());
        if (!animation_warning_.empty()) {
            ui::TextEllipsis(animation_warning_, 0, &ui::kWarning);
        }
    }

    // Cell padding is added below each row, so the cells leave room for it.
    const float height = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().CellPadding.y * 2;
    if (ImGui::BeginTable("preview_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX,
            {0, height})) {
        ImGui::TableSetupColumn("viewport", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthFixed, 260 * scale);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        viewport_.Draw("viewport", {ImGui::GetContentRegionAvail().x, height}, camera_);
        if (viewport_.TakeContextMenu()) {
            ImGui::OpenPopup("view_menu");
        }
        if (ImGui::BeginPopup("view_menu")) {
            bool menu_frame = false;
            if (DrawViewMenu(settings_, scene.LodCount, menu_frame)) {
                viewport_.SetLod(settings_.Lod);
            }
            frame = frame || menu_frame;
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        ImGui::BeginChild("details", {0, height});
        DrawDetails();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    if (frame) {
        const OrbitCamera framed = OrbitCamera::Framing(scene.Min, scene.Max);
        camera_.Target = framed.Target;
        camera_.Distance = framed.Distance;
    }
}

void PreviewPanel::Record(VkCommandBuffer command_buffer) {
    ZoneScopedN("Record preview");
    viewport_.Record(command_buffer, camera_, settings_);
}

}  // namespace skeleton_repair
