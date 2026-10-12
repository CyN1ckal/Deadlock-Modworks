#include "skeleton_pose.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using skeleton_repair::BoneTransform;
using skeleton_repair::GeometryBone;
using skeleton_repair::Mat34;
using skeleton_repair::Quat;
using skeleton_repair::SkeletonPoser;
using skeleton_repair::Vec3;

// Bones whose model-space pose is a plain offset from the origin, so parent-space translations are differences.
GeometryBone Bone(const char* name, int parent, Vec3 local, Vec3 model) {
    GeometryBone bone;
    bone.Name = name;
    bone.Parent = parent;
    bone.LocalPosition = local;
    bone.ModelPosition = model;
    return bone;
}

// root at the origin, spine 10 up, head 20 up.
std::vector<GeometryBone> Spine(float spine_height = 10) {
    return {Bone("root", -1, {0, 0, 0}, {0, 0, 0}), Bone("spine", 0, {0, 0, spine_height}, {0, 0, spine_height}),
        Bone("head", 1, {0, 0, 10}, {0, 0, spine_height + 10})};
}

Vec3 Apply(const Mat34& m, Vec3 p) {
    return Vec3{m[0] * p.X + m[1] * p.Y + m[2] * p.Z + m[3], m[4] * p.X + m[5] * p.Y + m[6] * p.Z + m[7],
        m[8] * p.X + m[9] * p.Y + m[10] * p.Z + m[11]};
}

void ExpectNear(Vec3 actual, Vec3 expected) {
    EXPECT_NEAR(actual.X, expected.X, 1e-4f);
    EXPECT_NEAR(actual.Y, expected.Y, 1e-4f);
    EXPECT_NEAR(actual.Z, expected.Z, 1e-4f);
}

TEST(SkeletonPoser, ReferencePoseLeavesTheMeshAlone) {
    const SkeletonPoser poser(Spine(), Spine());
    EXPECT_EQ(poser.MatchedBones(), 3u);
    const auto posed = poser.Pose(poser.ReferencePose());
    ASSERT_EQ(posed.Skinning.size(), 3u);
    for (const Mat34& m : posed.Skinning) {
        ExpectNear(Apply(m, {3, 4, 5}), {3, 4, 5});
    }
    ExpectNear(posed.Bones[2].ModelPosition, {0, 0, 20});
}

TEST(SkeletonPoser, RotatingTheRootCarriesTheChildren) {
    const SkeletonPoser poser(Spine(), Spine());
    auto pose = poser.ReferencePose();
    // A quarter turn about X tips the spine from +Z to -Y.
    pose[0].Rotation = Quat{0.70710678f, 0, 0, 0.70710678f};
    const auto posed = poser.Pose(pose);
    ExpectNear(posed.Bones[2].ModelPosition, {0, -20, 0});
    ExpectNear(posed.AnimationBones[2].ModelPosition, {0, -20, 0});
    // A vertex at the head moves with it.
    ExpectNear(Apply(posed.Skinning[2], {0, 0, 21}), {0, -21, 0});
}

TEST(SkeletonPoser, UnmatchedBonesFollowTheirParent) {
    std::vector<GeometryBone> render = Spine();
    render.push_back(Bone("cloth", 1, {5, 0, 0}, {5, 0, 10}));
    const SkeletonPoser poser(render, Spine());
    EXPECT_EQ(poser.MatchedBones(), 3u);
    auto pose = poser.ReferencePose();
    pose[1].Translation = {0, 0, 15};  // raise the spine by 5
    const auto posed = poser.Pose(pose);
    ExpectNear(posed.Bones[3].ModelPosition, {5, 0, 15});
    ExpectNear(Apply(posed.Skinning[3], {6, 0, 10}), {6, 0, 15});
}

TEST(SkeletonPoser, DifferentProportionsMoveTheBoundMesh) {
    // The mod's mesh was bound to a spine 10 up; the animation skeleton puts it 12 up.
    const SkeletonPoser poser(Spine(10), Spine(12));
    const auto posed = poser.Pose(poser.ReferencePose());
    ExpectNear(Apply(posed.Skinning[1], {0, 0, 10}), {0, 0, 12});
    ExpectNear(Apply(posed.Skinning[0], {1, 1, 1}), {1, 1, 1});
}

TEST(SkeletonPoser, ShortPosesFallBackToTheReference) {
    const SkeletonPoser poser(Spine(), Spine());
    std::vector<BoneTransform> pose = poser.ReferencePose();
    pose.resize(1);
    const auto posed = poser.Pose(pose);
    ExpectNear(posed.Bones[2].ModelPosition, {0, 0, 20});
}

}  // namespace
