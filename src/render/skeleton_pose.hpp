#pragma once

#include "bone_transform.hpp"
#include "model_geometry.hpp"

#include <array>
#include <span>
#include <vector>

namespace skeleton_repair {

// Row-major 3x4 affine matrix: three rows of {x, y, z, translation}, as the skinning shader reads them.
using Mat34 = std::array<float, 12>;

struct PosedSkeleton {
    // One per render bone: the posed model-space transform times the inverse bind transform.
    std::vector<Mat34> Skinning;
    // The render bones with posed model-space positions and rotations, for drawing the skeleton.
    std::vector<GeometryBone> Bones;
    // The animation skeleton's bones, posed the same way.
    std::vector<GeometryBone> AnimationBones;
};

// Drives a model's render skeleton from an animation skeleton, matching bones by name as the game does.
// Render bones the animation skeleton lacks keep their bind offset from their parent.
class SkeletonPoser {
public:
    SkeletonPoser(const std::vector<GeometryBone>& render, const std::vector<GeometryBone>& animation);

    size_t MatchedBones() const { return matched_; }
    size_t AnimationBoneCount() const { return animation_.size(); }

    // Parent-space reference pose of the animation skeleton: the base for additive clips.
    std::vector<BoneTransform> ReferencePose() const;

    // `local` holds parent-space transforms in animation-skeleton order. Bones past its end use the reference pose.
    PosedSkeleton Pose(std::span<const BoneTransform> local) const;

private:
    std::vector<GeometryBone> render_;
    std::vector<GeometryBone> animation_;
    // Animation bone index per render bone, or -1.
    std::vector<int> source_;
    size_t matched_ = 0;
};

}  // namespace skeleton_repair
