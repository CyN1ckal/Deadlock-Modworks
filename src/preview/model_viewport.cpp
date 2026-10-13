#include "profile.hpp"
#include "model_viewport.hpp"
#include "theme.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace skeleton_repair {

namespace {
struct ColorModeOption {
    const char* Label;
    ColorMode Value;
};
constexpr ColorModeOption kColorModes[] = {{"Textured", ColorMode::Textured}, {"Clay", ColorMode::Clay},
    {"Material colors", ColorMode::Material}, {"Bone colors", ColorMode::Bone}};
}  // namespace

bool DrawViewSettings(ViewSettings& settings, int lod_count, bool& frame) {
    const float scale = ImGui::GetFontSize() / 15.0f;
    bool lod_changed = false;
    if (lod_count > 1) {
        ImGui::SetNextItemWidth(64 * scale);
        lod_changed = ImGui::SliderInt("##lod", &settings.Lod, 0, lod_count - 1, "LOD %d");
        ImGui::SetItemTooltip("Level of detail. 0 is the full-quality mesh.");
        ImGui::SameLine();
    }
    ImGui::Checkbox("Bones", &settings.ShowSkeleton);
    ImGui::SetItemTooltip("Draw the skeleton over the model");
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &settings.ShowGrid);
    ImGui::SameLine();
    const char* current = kColorModes[0].Label;
    for (const ColorModeOption& mode : kColorModes) {
        if (mode.Value == settings.Coloring) {
            current = mode.Label;
        }
    }
    ImGui::SetNextItemWidth(118 * scale);
    if (ImGui::BeginCombo("##coloring", current)) {
        for (const ColorModeOption& mode : kColorModes) {
            if (ImGui::Selectable(mode.Label, mode.Value == settings.Coloring)) {
                settings.Coloring = mode.Value;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Textured: material color maps. Bone colors: each vertex takes the color of the bone that moves it most.");
    ImGui::SameLine();
    frame = ImGui::SmallButton("Frame");
    ImGui::SetItemTooltip("Fit the model in view (F over the viewport)");
    return lod_changed;
}

bool DrawViewMenu(ViewSettings& settings, int lod_count, bool& frame) {
    bool lod_changed = false;
    frame = ImGui::MenuItem("Frame model", "F");
    ImGui::Separator();
    ImGui::MenuItem("Show bones", nullptr, &settings.ShowSkeleton);
    ImGui::MenuItem("Show grid", nullptr, &settings.ShowGrid);
    ImGui::Separator();
    for (const ColorModeOption& mode : kColorModes) {
        if (ImGui::MenuItem(mode.Label, nullptr, mode.Value == settings.Coloring)) {
            settings.Coloring = mode.Value;
        }
    }
    if (lod_count > 1 && ImGui::BeginMenu("Level of detail")) {
        for (int lod = 0; lod < lod_count; ++lod) {
            if (ImGui::MenuItem(("LOD " + std::to_string(lod)).c_str(), nullptr, settings.Lod == lod)) {
                lod_changed = settings.Lod != lod;
                settings.Lod = lod;
            }
        }
        ImGui::EndMenu();
    }
    return lod_changed;
}

ModelViewport::ModelViewport(const VulkanDevice& device) : renderer_(device) {}

ModelViewport::~ModelViewport() {
    if (texture_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture_);
    }
}

PreparedModel PrepareModel(const ModelGeometry& geometry, const ResourceReader* read, uint32_t max_texture_size,
    const PreparedModel* reuse) {
    PreparedModel prepared;
    prepared.Bones = geometry.Bones;
    prepared.Scene = BuildPreviewScene(geometry);
    if (read == nullptr) {
        return prepared;
    }
    const auto names = [](const PreviewScene& scene) {
        std::vector<std::string> materials;
        for (const PreviewDraw& draw : scene.Draws) {
            if (!draw.Material.empty() && std::find(materials.begin(), materials.end(), draw.Material) == materials.end()) {
                materials.push_back(draw.Material);
            }
        }
        return materials;
    };
    if (reuse != nullptr && reuse->Materials && names(reuse->Scene) == names(prepared.Scene)) {
        prepared.Materials = reuse->Materials;
    } else {
        prepared.Materials = std::make_shared<const std::vector<PreviewMaterial>>(LoadPreviewMaterials(prepared.Scene, *read, max_texture_size));
    }
    return prepared;
}

void ModelViewport::SetModel(const ModelGeometry* geometry, int lod, const ResourceReader* read, uint32_t max_texture_size) {
    if (geometry == nullptr) {
        SetModel(static_cast<const PreparedModel*>(nullptr), lod);
        return;
    }
    const PreparedModel prepared = PrepareModel(*geometry, read, max_texture_size);
    SetModel(&prepared, lod);
}

void ModelViewport::SetModel(const PreparedModel* model, int lod) {
    textured_materials_ = 0;
    material_count_ = 0;
    material_notes_.clear();
    poser_.reset();
    posed_.reset();
    overlay_colors_.clear();
    overlay_animation_ = false;
    if (model == nullptr) {
        renderer_.ClearScene();
        scene_ = {};
        model_bones_.clear();
        visible_.clear();
        return;
    }
    model_bones_ = model->Bones;
    scene_ = model->Scene;
    renderer_.SetScene(scene_);
    SetLod(lod);
    if (!model->Materials) {
        return;
    }
    const std::vector<PreviewMaterial>& materials = *model->Materials;
    material_notes_ = renderer_.SetMaterials(materials);
    material_count_ = materials.size();
    textured_materials_ = renderer_.TextureCount();
    for (const PreviewMaterial& material : materials) {
        if (!material.Problem.empty()) {
            material_notes_.push_back(material.Name + ": " + material.Problem);
        }
    }
}

void ModelViewport::SetSkeleton(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors, bool animation) {
    overlay_colors_.assign(colors.begin(), colors.end());
    overlay_animation_ = animation;
    scene_.Skeleton = SkeletonLines(bones, colors);
    renderer_.SetSkeleton(scene_.Skeleton);
}

void ModelViewport::SetAnimationSkeleton(const std::vector<GeometryBone>* bones) {
    posed_.reset();
    if (bones == nullptr || bones->empty() || model_bones_.empty()) {
        poser_.reset();
        return;
    }
    poser_.emplace(model_bones_, *bones);
}

std::string ModelViewport::Animate(const AnimationClip* clip, float seconds) {
    if (clip == nullptr || !poser_) {
        posed_.reset();
        return {};
    }
    local_ = poser_->ReferencePose();
    std::string warning;
    if (clip->Tracks.size() != local_.size()) {
        // The game samples tracks by index too, so a stale skeleton is posed the same wrong way.
        warning = "The clip has " + std::to_string(clip->Tracks.size()) + " tracks but this animation skeleton has "
            + std::to_string(local_.size()) + " bones, so bones are driven by the wrong tracks.";
    }
    if (clip->Additive) {
        std::vector<BoneTransform> delta(local_.size());
        clip->Sample(seconds, delta);
        for (size_t i = 0; i < std::min(local_.size(), clip->Tracks.size()); ++i) {
            local_[i] = ApplyAdditive(local_[i], delta[i]);
        }
    } else {
        clip->Sample(seconds, local_);
    }
    posed_ = poser_->Pose(local_);
    posed_lines_ = SkeletonLines(overlay_animation_ ? posed_->AnimationBones : posed_->Bones, overlay_colors_);
    return warning;
}

void ModelViewport::SetLod(int lod) {
    visible_ = DefaultVisibility(scene_, std::clamp(lod, 0, std::max(scene_.LodCount - 1, 0)));
}

bool ModelViewport::Draw(const char* id, ImVec2 size, OrbitCamera& camera, bool interactive) {
    ZoneScopedN("Viewport");
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
    if (!interactive) {
        ImGui::Dummy(size);
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture_)), origin,
            ImVec2(origin.x + size.x, origin.y + size.y));
        drawn_ = true;
        return false;
    }
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
        // A right drag pans, so only a click opens the menu.
        const float threshold = io.MouseDragThreshold;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < threshold * threshold) {
            context_requested_ = true;
        }
    }
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture_)), origin, ImVec2(origin.x + size.x, origin.y + size.y));
    if (ImGui::IsItemHovered()) {
        constexpr const char* kHint = "Drag: orbit   Right/middle drag: pan   Wheel: zoom   F: frame   Right-click: options";
        const float line = ImGui::GetTextLineHeight();
        draw->AddText({origin.x + 6, origin.y + size.y - line - 4}, ImGui::GetColorU32(ImGuiCol_TextDisabled), kHint);
    }
    drawn_ = true;
    return moved;
}

void ModelViewport::Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const ViewSettings& settings) {
    ZoneScopedN("Record viewport");
    if (!drawn_) {
        return;
    }
    RenderOptions options;
    options.Background = theme::kClearRgb;
    options.VisibleMeshes = visible_;
    options.ShowSkeleton = settings.ShowSkeleton;
    options.ShowGrid = settings.ShowGrid;
    options.Coloring = settings.Coloring;
    FramePose pose;
    if (posed_) {
        pose.Skinning = posed_->Skinning;
        pose.Skeleton = posed_lines_;
        options.Pose = &pose;
    }
    renderer_.Record(command_buffer, camera, options);
    drawn_ = false;
}

}  // namespace skeleton_repair
