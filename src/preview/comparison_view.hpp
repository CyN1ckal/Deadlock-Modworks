#pragma once

#include "model_comparison.hpp"
#include "model_versions.hpp"
#include "model_viewport.hpp"

#include <vulkan/vulkan.h>

#include <functional>
#include <optional>
#include <string>

namespace skeleton_repair {

enum class SkeletonKind {
    // m_modelSkeleton, which the mesh is skinned to.
    Render,
    // The AG2 .vnmskel animations play on.
    Animation,
};

// Two model versions side by side with linked cameras, a skeleton diff and a skin-binding check.
class ComparisonView {
public:
    explicit ComparisonView(const VulkanDevice& device);

    // Reads a compiled resource as a given version sees it: the mod archive first for Mod and Repaired, the game for Game.
    using VersionReader = std::function<std::optional<std::vector<uint8_t>>(ModelVersion version, const std::string& path)>;

    void Load(ModelVersions versions, VersionReader read = {}, std::vector<std::string> clips = {});
    void ShowError(std::string title, std::string message);
    void PlayClip(const std::string& path);
    bool Loaded() const { return loaded_; }

    void Draw();
    // Records both offscreen passes. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer);

private:
    void Refresh(bool reframe);
    const std::vector<GeometryBone>* Skeleton(ModelVersion version) const;
    void DrawReport();
    void DrawDiff();

    void ShowSide(ModelViewport& view, ModelVersion version, std::optional<ModelVersion>& shown);

    ModelVersions versions_;
    VersionReader read_;
    std::optional<ModelVersion> left_shown_;
    std::optional<ModelVersion> right_shown_;
    bool loaded_ = false;
    std::string title_;
    std::string error_;
    ModelVersion left_ = ModelVersion::Mod;
    ModelVersion right_ = ModelVersion::Repaired;
    SkeletonKind kind_ = SkeletonKind::Animation;
    bool link_cameras_ = true;
    ViewSettings settings_;
    ModelViewport left_view_;
    ModelViewport right_view_;
    AnimationPlayer player_;
    std::vector<std::string> clips_;
    std::string left_warning_;
    std::string right_warning_;
    OrbitCamera left_camera_;
    OrbitCamera right_camera_;
    SkeletonDiff skeleton_diff_;
    BindingDiff binding_diff_;
    bool show_unchanged_ = false;
};

}  // namespace skeleton_repair
