#include "comparison_view.hpp"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace skeleton_repair {
namespace {

ImVec4 Unpack(uint32_t rgba) {
    return ImVec4((rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, 1.0f);
}

ImVec4 OutcomeColor(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Updated:
    case ModelOutcome::Current: return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    case ModelOutcome::Repairable: return ImVec4(0.4f, 0.8f, 1.0f, 1.0f);
    case ModelOutcome::NeedsReview: return ImVec4(1.0f, 0.65f, 0.25f, 1.0f);
    case ModelOutcome::Failed: return ImVec4(1.0f, 0.4f, 0.35f, 1.0f);
    case ModelOutcome::Skipped: break;
    }
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

const char* ChangeName(BoneChange change) {
    switch (change) {
    case BoneChange::Same: return "same";
    case BoneChange::Moved: return "moved";
    case BoneChange::Reparented: return "reparented";
    case BoneChange::Added: return "added";
    case BoneChange::Removed: return "removed";
    }
    return "";
}

uint32_t ChangeColor(BoneChange change) {
    switch (change) {
    case BoneChange::Moved: return kMovedBoneColor;
    case BoneChange::Reparented: return kReparentedBoneColor;
    case BoneChange::Added: return kAddedBoneColor;
    case BoneChange::Removed: return kRemovedBoneColor;
    case BoneChange::Same: break;
    }
    return kBoneLineColor;
}

void Swatch(uint32_t color, const char* label) {
    ImGui::TextColored(Unpack(color), "\xE2\x96\xA0");  // U+25A0 black square
    ImGui::SameLine(0, 4);
    ImGui::TextUnformatted(label);
}

void DrawEdit(const char* what, const MigrationEdit& edit) {
    ImGui::TextUnformatted(what);
    ImGui::SameLine();
    ImGui::TextColored(OutcomeColor(edit.Outcome), "%s", ModelOutcomeName(edit.Outcome));
    ImGui::Indent();
    if (!edit.Detail.empty()) {
        ImGui::TextWrapped("%s", edit.Detail.c_str());
    }
    if (edit.Warnings) {
        for (const std::string& warning : *edit.Warnings) {
            ImGui::BulletText("%s", warning.c_str());
        }
    }
    ImGui::Unindent();
}

}  // namespace

ComparisonView::ComparisonView(const VulkanDevice& device) : left_view_(device), right_view_(device) {}

void ComparisonView::Load(ModelVersions versions, VersionReader read, std::vector<std::string> clips) {
    versions_ = std::move(versions);
    read_ = std::move(read);
    clips_ = std::move(clips);
    player_.SetClips(read_ ? clips_ : std::vector<std::string>{});
    left_shown_.reset();
    right_shown_.reset();
    loaded_ = true;
    error_.clear();
    title_ = versions_.ModelPath;

    const bool repaired_changes = (versions_.ModelEdit && versions_.ModelEdit->Bytes) || (versions_.SkeletonEdit && versions_.SkeletonEdit->Bytes);
    left_ = ModelVersion::Mod;
    if (repaired_changes || !versions_.Get(ModelVersion::Game).Available) {
        right_ = ModelVersion::Repaired;
    } else {
        // Nothing was rewritten, so the informative comparison is against the game.
        right_ = ModelVersion::Game;
    }
    kind_ = versions_.Get(ModelVersion::Mod).AnimationSkeleton ? SkeletonKind::Animation : SkeletonKind::Render;
    settings_.Lod = 0;
    Refresh(true);
}

void ComparisonView::PlayClip(const std::string& path) {
    if (read_) {
        player_.Select(path, [this](const std::string& resource) { return read_(ModelVersion::Mod, resource); });
    }
}

void ComparisonView::ShowError(std::string title, std::string message) {
    versions_ = {};
    read_ = {};
    loaded_ = false;
    title_ = std::move(title);
    error_ = std::move(message);
    left_view_.SetModel(nullptr, 0);
    right_view_.SetModel(nullptr, 0);
    left_shown_.reset();
    right_shown_.reset();
}

const std::vector<GeometryBone>* ComparisonView::Skeleton(ModelVersion version) const {
    const VersionData& data = versions_.Get(version);
    if (!data.Available) {
        return nullptr;
    }
    if (kind_ == SkeletonKind::Animation) {
        return data.AnimationSkeleton ? &*data.AnimationSkeleton : nullptr;
    }
    return data.Model ? &data.Model->Bones : nullptr;
}

void ComparisonView::ShowSide(ModelViewport& view, ModelVersion version, std::optional<ModelVersion>& shown) {
    // Loading materials reads every color map, so only reload when the side shows another version.
    if (shown == version) {
        return;
    }
    shown = version;
    const VersionData& data = versions_.Get(version);
    const ModelGeometry* model = data.Available && data.Model ? &*data.Model : nullptr;
    if (!read_) {
        view.SetModel(model, settings_.Lod);
        return;
    }
    const ResourceReader read = [this, version](const std::string& path) { return read_(version, path); };
    view.SetModel(model, settings_.Lod, &read);
}

void ComparisonView::Refresh(bool reframe) {
    const VersionData& left = versions_.Get(left_);
    const VersionData& right = versions_.Get(right_);
    const ModelGeometry* left_model = left.Available && left.Model ? &*left.Model : nullptr;
    const ModelGeometry* right_model = right.Available && right.Model ? &*right.Model : nullptr;
    ShowSide(left_view_, left_, left_shown_);
    ShowSide(right_view_, right_, right_shown_);

    const std::vector<GeometryBone>* left_bones = Skeleton(left_);
    const std::vector<GeometryBone>* right_bones = Skeleton(right_);
    static const std::vector<GeometryBone> kNone;
    skeleton_diff_ = left_bones && right_bones ? CompareSkeletons(*left_bones, *right_bones) : SkeletonDiff{};
    const bool animation = kind_ == SkeletonKind::Animation;
    if (left_model || left_bones) {
        const auto& bones = left_bones ? *left_bones : kNone;
        left_view_.SetSkeleton(bones, right_bones ? DiffColors(bones, skeleton_diff_, true) : std::vector<uint32_t>{}, animation);
    }
    if (right_model || right_bones) {
        const auto& bones = right_bones ? *right_bones : kNone;
        right_view_.SetSkeleton(bones, left_bones ? DiffColors(bones, skeleton_diff_, false) : std::vector<uint32_t>{}, animation);
    }
    // Each side plays clips on the animation skeleton the game would use for that version.
    const auto& left_animation = versions_.Get(left_).AnimationSkeleton;
    const auto& right_animation = versions_.Get(right_).AnimationSkeleton;
    left_view_.SetAnimationSkeleton(left_animation ? &*left_animation : nullptr);
    right_view_.SetAnimationSkeleton(right_animation ? &*right_animation : nullptr);

    if (left_model && right_model) {
        binding_diff_ = CompareBindings(*left_model, *right_model);
    } else {
        binding_diff_ = {};
        binding_diff_.Reason = "One side has no model.";
    }

    if (reframe) {
        const PreviewScene& scene = left_view_.Empty() ? right_view_.Scene() : left_view_.Scene();
        left_camera_ = OrbitCamera::Framing(scene.Min, scene.Max);
        right_camera_ = left_camera_;
    }
}

void ComparisonView::DrawReport() {
    if (!ImGui::CollapsingHeader("Repair", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (versions_.ModelEdit) {
        DrawEdit("Model:", *versions_.ModelEdit);
    }
    if (versions_.SkeletonEdit) {
        DrawEdit("Animation skeleton:", *versions_.SkeletonEdit);
    }
    for (ModelVersion version : {left_, right_}) {
        const VersionData& data = versions_.Get(version);
        for (const std::string& note : data.Notes) {
            ImGui::TextDisabled("%s: %s", ModelVersionName(version), note.c_str());
        }
        if (!data.Error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s: %s", ModelVersionName(version), data.Error.c_str());
        }
    }
    ImGui::TextDisabled("The preview runs the model and animation-skeleton migrations only; a full update also fixes dependent files.");
}

void ComparisonView::DrawDiff() {
    const char* kind = kind_ == SkeletonKind::Animation ? "Animation skeleton" : "Render skeleton";
    if (!Skeleton(left_) || !Skeleton(right_)) {
        ImGui::TextDisabled("%s: not available on both sides.", kind);
    } else if (skeleton_diff_.Identical()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "%s: identical (%zu bones).", kind, skeleton_diff_.Same);
    } else {
        ImGui::Text("%s:", kind);
        ImGui::SameLine();
        const std::pair<size_t, BoneChange> counts[] = {{skeleton_diff_.Added, BoneChange::Added},
            {skeleton_diff_.Moved, BoneChange::Moved}, {skeleton_diff_.Reparented, BoneChange::Reparented},
            {skeleton_diff_.Removed, BoneChange::Removed}};
        for (const auto& [count, change] : counts) {
            ImGui::TextColored(Unpack(ChangeColor(change)), "%zu %s", count, ChangeName(change));
            ImGui::SameLine();
        }
        ImGui::TextDisabled("%zu unchanged", skeleton_diff_.Same);
    }

    if (!binding_diff_.Comparable) {
        ImGui::TextDisabled("Skin bindings: not compared. %s", binding_diff_.Reason.c_str());
    } else if (binding_diff_.Changed == 0) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Skin bindings: every vertex keeps its bones and weights (%llu vertices).",
            static_cast<unsigned long long>(binding_diff_.Vertices));
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Skin bindings: %llu of %llu vertices are driven by different bones.",
            static_cast<unsigned long long>(binding_diff_.Changed), static_cast<unsigned long long>(binding_diff_.Vertices));
        for (const std::string& example : binding_diff_.Examples) {
            ImGui::BulletText("%s", example.c_str());
        }
    }

    const size_t changed = skeleton_diff_.Bones.size() - skeleton_diff_.Same;
    if (skeleton_diff_.Bones.empty() || !ImGui::CollapsingHeader(("Bone changes (" + std::to_string(changed) + ")###bones").c_str())) {
        return;
    }
    Swatch(kAddedBoneColor, "added");
    ImGui::SameLine();
    Swatch(kMovedBoneColor, "moved");
    ImGui::SameLine();
    Swatch(kReparentedBoneColor, "reparented");
    ImGui::SameLine();
    Swatch(kRemovedBoneColor, "removed (left side)");
    ImGui::SameLine();
    ImGui::Checkbox("Show unchanged", &show_unchanged_);

    std::vector<const BoneDiff*> rows;
    for (const BoneDiff& bone : skeleton_diff_.Bones) {
        if (show_unchanged_ || bone.Change != BoneChange::Same) {
            rows.push_back(&bone);
        }
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("bone_changes", 4, flags, ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 9))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Bone");
        ImGui::TableSetupColumn("Change", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Moved by", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Parent (left -> right)");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const BoneDiff& bone = *rows[static_cast<size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(bone.Name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(Unpack(ChangeColor(bone.Change)), "%s", ChangeName(bone.Change));
                ImGui::TableNextColumn();
                if (bone.Change != BoneChange::Added && bone.Change != BoneChange::Removed) {
                    ImGui::Text("%.3f", bone.Distance);
                }
                ImGui::TableNextColumn();
                if (bone.LeftParent == bone.RightParent) {
                    ImGui::TextUnformatted(bone.LeftParent.c_str());
                } else {
                    ImGui::Text("%s -> %s", bone.LeftParent.empty() ? "-" : bone.LeftParent.c_str(),
                        bone.RightParent.empty() ? "-" : bone.RightParent.c_str());
                }
            }
        }
        ImGui::EndTable();
    }
}

void ComparisonView::Draw() {
    ImGui::TextUnformatted(title_.empty() ? "Open a mod VPK and pick a model to compare it with the game." : title_.c_str());
    if (!error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", error_.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (!loaded_) {
        return;
    }

    bool changed = false;
    auto version_combo = [&](const char* label, ModelVersion& version) {
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo(label, ModelVersionName(version))) {
            for (ModelVersion option : {ModelVersion::Mod, ModelVersion::Repaired, ModelVersion::Game}) {
                const bool available = versions_.Get(option).Available;
                if (ImGui::Selectable(ModelVersionName(option), option == version, available ? 0 : ImGuiSelectableFlags_Disabled)) {
                    changed = changed || option != version;
                    version = option;
                }
            }
            ImGui::EndCombo();
        }
    };
    version_combo("Left", left_);
    ImGui::SameLine();
    version_combo("Right", right_);
    ImGui::SameLine();
    int kind = static_cast<int>(kind_);
    changed = ImGui::RadioButton("Render skeleton", &kind, 0) || changed;
    ImGui::SetItemTooltip("m_modelSkeleton: the bones the mesh is skinned to.");
    ImGui::SameLine();
    changed = ImGui::RadioButton("Animation skeleton", &kind, 1) || changed;
    ImGui::SetItemTooltip("The AG2 .vnmskel that animations play on.");
    kind_ = static_cast<SkeletonKind>(kind);
    ImGui::SameLine();
    ImGui::Checkbox("Link cameras", &link_cameras_);
    if (changed) {
        Refresh(false);
    }

    bool frame = false;
    const int lod_count = std::max(left_view_.Scene().LodCount, right_view_.Scene().LodCount);
    if (DrawViewSettings(settings_, lod_count, frame)) {
        left_view_.SetLod(settings_.Lod);
        right_view_.SetLod(settings_.Lod);
    }
    if (frame) {
        const PreviewScene& scene = left_view_.Empty() ? right_view_.Scene() : left_view_.Scene();
        left_camera_ = OrbitCamera::Framing(scene.Min, scene.Max);
        right_camera_ = left_camera_;
    }

    if (read_ && (left_view_.CanAnimate() || right_view_.CanAnimate())) {
        player_.Draw([this](const std::string& path) { return read_(ModelVersion::Mod, path); });
        player_.Advance(ImGui::GetIO().DeltaTime);
        left_warning_ = left_view_.Animate(player_.Clip(), player_.Time());
        right_warning_ = right_view_.Animate(player_.Clip(), player_.Time());
    }

    DrawReport();
    DrawDiff();

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 size{(available.x - spacing) * 0.5f, available.y - ImGui::GetTextLineHeightWithSpacing()};
    auto column = [&](const char* id, ModelVersion version, ModelViewport& view, OrbitCamera& camera, const std::string& warning) {
        ImGui::BeginGroup();
        ImGui::TextUnformatted(ModelVersionName(version));
        if (!warning.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), "(clip mismatch)");
            ImGui::SetItemTooltip("%s", warning.c_str());
        }
        if (view.Empty()) {
            ImGui::Dummy(size);
        } else if (view.Draw(id, size, camera) && link_cameras_) {
            // Whichever side the user moves becomes the shared view.
            left_camera_ = camera;
            right_camera_ = camera;
        }
        ImGui::EndGroup();
    };
    column("left_viewport", left_, left_view_, left_camera_, left_warning_);
    ImGui::SameLine();
    column("right_viewport", right_, right_view_, right_camera_, right_warning_);
}

void ComparisonView::Record(VkCommandBuffer command_buffer) {
    if (!loaded_) {
        return;
    }
    left_view_.Record(command_buffer, left_camera_, settings_);
    right_view_.Record(command_buffer, right_camera_, settings_);
}

}  // namespace skeleton_repair
