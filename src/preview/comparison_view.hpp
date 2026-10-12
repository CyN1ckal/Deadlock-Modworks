#pragma once

#include "model_comparison.hpp"
#include "model_versions.hpp"
#include "model_viewport.hpp"

#include <vulkan/vulkan.h>

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

    void Load(ModelVersions versions);
    void ShowError(std::string title, std::string message);
    bool Loaded() const { return loaded_; }

    void Draw();
    // Records both offscreen passes. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer);

private:
    void Refresh(bool reframe);
    const std::vector<GeometryBone>* Skeleton(ModelVersion version) const;
    void DrawReport();
    void DrawDiff();

    ModelVersions versions_;
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
    OrbitCamera left_camera_;
    OrbitCamera right_camera_;
    SkeletonDiff skeleton_diff_;
    BindingDiff binding_diff_;
    bool show_unchanged_ = false;
};

}  // namespace skeleton_repair
