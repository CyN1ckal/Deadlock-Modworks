#include "model_viewport.hpp"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>

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
    struct Mode {
        const char* Label;
        ColorMode Value;
    };
    static const Mode kModes[] = {{"Textured", ColorMode::Textured}, {"Clay", ColorMode::Clay},
        {"Material colors", ColorMode::Material}, {"Bone colors", ColorMode::Bone}};
    const char* current = kModes[0].Label;
    for (const Mode& mode : kModes) {
        if (mode.Value == settings.Coloring) {
            current = mode.Label;
        }
    }
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##coloring", current)) {
        for (const Mode& mode : kModes) {
            if (ImGui::Selectable(mode.Label, mode.Value == settings.Coloring)) {
                settings.Coloring = mode.Value;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Textured: material color maps. Bone colors: each vertex takes the color of the bone that moves it most.");
    ImGui::SameLine();
    frame = ImGui::Button("Frame");
    return lod_changed;
}

void AnimationPlayer::SetClips(const std::vector<std::string>& paths) {
    constexpr std::string_view kTwin = ".vnmclip+non_additive.vnmclip_c";
    constexpr std::string_view kClip = ".vnmclip_c";
    std::vector<std::string> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    clips_.clear();
    loading_.Cancel();
    for (const std::string& path : sorted) {
        const bool twin = path.size() > kTwin.size() && path.compare(path.size() - kTwin.size(), kTwin.size(), kTwin) == 0;
        std::string base = twin ? path.substr(0, path.size() - kTwin.size()) : path.substr(0, path.size() - kClip.size());
        if (!twin && std::binary_search(sorted.begin(), sorted.end(), base + std::string(kTwin))) {
            continue;
        }
        const size_t slash = base.rfind('/');
        clips_.push_back({path, slash == std::string::npos ? base : base.substr(slash + 1)});
    }
    selected_ = -1;
    clip_.reset();
    error_.clear();
    time_ = 0;
}

bool AnimationPlayer::Draw(const ResourceReader& read) {
    bool changed = Poll();
    ImGui::SetNextItemWidth(260);
    const char* preview = selected_ >= 0 ? clips_[static_cast<size_t>(selected_)].Label.c_str() : "Bind pose";
    if (ImGui::BeginCombo("##clip", clips_.empty() ? "No animation clips" : preview, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##clip_filter", "Filter", filter_, sizeof(filter_));
        if (ImGui::Selectable("Bind pose", selected_ < 0)) {
            selected_ = -1;
            loading_.Cancel();
            clip_.reset();
            error_.clear();
            changed = true;
        }
        for (size_t i = 0; i < clips_.size(); ++i) {
            if (filter_[0] != '\0' && clips_[i].Label.find(filter_) == std::string::npos) {
                continue;
            }
            if (ImGui::Selectable(clips_[i].Label.c_str(), static_cast<int>(i) == selected_)) {
                Select(clips_[i].Path, read);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "Pause" : "Play")) {
        playing_ = !playing_;
    }
    if (clip_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        float shown = clip_->Duration > 0 ? std::fmod(time_, clip_->Duration) : 0;
        if (ImGui::SliderFloat("##time", &shown, 0, std::max(clip_->Duration, 0.001f), "%.2f s")) {
            time_ = shown;
            playing_ = false;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::SliderFloat("Speed", &speed_, 0.1f, 2.0f, "%.1fx");
        ImGui::SameLine();
        ImGui::TextDisabled("%u frames%s", clip_->FrameCount, clip_->Additive ? ", additive" : "");
    }
    if (loading_.Busy()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Loading clip...");
    }
    if (!error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());
    }
    return changed;
}

bool AnimationPlayer::Select(const std::string& path, const ResourceReader& read) {
    const auto found = std::find_if(clips_.begin(), clips_.end(), [&](const Entry& entry) { return entry.Path == path; });
    if (found == clips_.end()) {
        return false;
    }
    selected_ = static_cast<int>(found - clips_.begin());
    clip_.reset();
    error_.clear();
    time_ = 0;
    playing_ = true;
    loading_.Start([read, path](CancellationToken) {
        const auto bytes = read(path);
        if (!bytes) {
            throw std::runtime_error("Clip is missing.");
        }
        return DecodeAnimationClip(*bytes);
    });
    return true;
}

bool AnimationPlayer::Poll() {
    try {
        if (auto clip = loading_.Take()) {
            clip_ = std::move(*clip);
            time_ = 0;
            return true;
        }
    } catch (const std::exception& error) {
        error_ = error.what();
    }
    return false;
}

void AnimationPlayer::Advance(float seconds) {
    if (playing_ && clip_) {
        time_ += seconds * speed_;
        if (clip_->Duration > 0) {
            time_ = std::fmod(time_, clip_->Duration);
        }
    }
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
