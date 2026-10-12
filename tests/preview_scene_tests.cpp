#include "preview_scene.hpp"
#include "preview_test_model.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using skeleton_repair::BuildPreviewScene;
using skeleton_repair::DefaultVisibility;
using skeleton_repair::Mat4;
using skeleton_repair::OrbitCamera;
using skeleton_repair::PreviewScene;
using skeleton_repair::Vec3;

struct Clip {
    float X, Y, Z;
};

Clip Project(const Mat4& m, Vec3 p) {
    const float x = m[0] * p.X + m[4] * p.Y + m[8] * p.Z + m[12];
    const float y = m[1] * p.X + m[5] * p.Y + m[9] * p.Z + m[13];
    const float z = m[2] * p.X + m[6] * p.Y + m[10] * p.Z + m[14];
    const float w = m[3] * p.X + m[7] * p.Y + m[11] * p.Z + m[15];
    return {x / w, y / w, z / w};
}

TEST(PreviewScene, FlattensMeshesIntoSharedBuffers) {
    const PreviewScene scene = BuildPreviewScene(preview_test::TwoLodModel());
    ASSERT_EQ(scene.Meshes.size(), 2u);
    EXPECT_EQ(scene.Vertices.size(), 8u);
    EXPECT_EQ(scene.Indices.size(), 12u);
    ASSERT_EQ(scene.Draws.size(), 2u);
    EXPECT_EQ(scene.Draws[1].FirstIndex, 6u);
    EXPECT_EQ(scene.Draws[1].VertexOffset, 4);
    EXPECT_EQ(scene.Draws[1].Mesh, 1u);
    EXPECT_EQ(scene.Meshes[0].TriangleCount, 2u);
    EXPECT_EQ(scene.Meshes[0].VertexCount, 4u);
    EXPECT_EQ(scene.LodCount, 2);
    EXPECT_EQ(scene.BoneCount, 2u);
    EXPECT_EQ(scene.Skeleton.size(), 2u);
    EXPECT_FALSE(scene.Grid.empty());
}

TEST(PreviewScene, DefaultVisibilityFollowsLodAndMeshGroup) {
    auto geometry = preview_test::TwoLodModel();
    PreviewScene scene = BuildPreviewScene(geometry);
    EXPECT_EQ(DefaultVisibility(scene, 0), (std::vector<uint8_t>{1, 0}));
    EXPECT_EQ(DefaultVisibility(scene, 1), (std::vector<uint8_t>{0, 1}));

    geometry.Meshes[0].MeshGroupMask = 2;
    scene = BuildPreviewScene(geometry);
    EXPECT_EQ(DefaultVisibility(scene, 0), (std::vector<uint8_t>{0, 0}));
}

TEST(PreviewScene, BoundsCoverDefaultLodOnly) {
    const PreviewScene scene = BuildPreviewScene(preview_test::TwoLodModel());
    // The LOD 1 quad is wider; it must not widen the framing of LOD 0.
    EXPECT_FLOAT_EQ(scene.Min.Y, -10);
    EXPECT_FLOAT_EQ(scene.Max.Y, 10);
    EXPECT_FLOAT_EQ(scene.Max.Z, 20);
}

TEST(PreviewScene, MaterialColorIsStablePerMaterial) {
    auto geometry = preview_test::TwoLodModel();
    geometry.Meshes[1].DrawCalls[0].Material = geometry.Meshes[0].DrawCalls[0].Material;
    const PreviewScene same = BuildPreviewScene(geometry);
    EXPECT_EQ(same.Draws[0].MaterialColor, same.Draws[1].MaterialColor);

    geometry.Meshes[1].DrawCalls[0].Material = "models/other.vmat";
    const PreviewScene different = BuildPreviewScene(geometry);
    EXPECT_NE(different.Draws[0].MaterialColor, different.Draws[1].MaterialColor);
}

TEST(PreviewScene, SkeletonOnlyModelFramesBones) {
    auto geometry = preview_test::TwoLodModel();
    geometry.Meshes.clear();
    const PreviewScene scene = BuildPreviewScene(geometry);
    EXPECT_FLOAT_EQ(scene.Min.Z, 0);
    EXPECT_FLOAT_EQ(scene.Max.Z, 18);
}

TEST(OrbitCamera, FramingCentersTheTarget) {
    const OrbitCamera camera = OrbitCamera::Framing({-10, -10, 0}, {10, 10, 20});
    EXPECT_FLOAT_EQ(camera.Target.Z, 10);
    const Clip center = Project(camera.ViewProjection(1.5f), camera.Target);
    EXPECT_NEAR(center.X, 0, 1e-5f);
    EXPECT_NEAR(center.Y, 0, 1e-5f);
    EXPECT_GT(center.Z, 0);
    EXPECT_LT(center.Z, 1);
    // The whole box fits on screen.
    for (Vec3 corner : {Vec3{-10, -10, 0}, Vec3{10, 10, 20}, Vec3{10, -10, 20}, Vec3{-10, 10, 0}}) {
        const Clip p = Project(camera.ViewProjection(1.0f), corner);
        EXPECT_LE(std::abs(p.X), 1.0f);
        EXPECT_LE(std::abs(p.Y), 1.0f);
    }
}

TEST(OrbitCamera, ZIsUpAndModelLeftIsScreenRight) {
    OrbitCamera camera;
    camera.Target = {0, 0, 0};
    camera.Pitch = 0;
    camera.Distance = 100;
    const Mat4 m = camera.ViewProjection(1.0f);
    // Vulkan clip space is Y down, so up is negative.
    EXPECT_LT(Project(m, {0, 0, 10}).Y, 0);
    // From the front (+X), the model's +Y (its left) is on the viewer's right.
    EXPECT_GT(Project(m, {0, 10, 0}).X, 0);
    // Nearer points have smaller depth.
    EXPECT_LT(Project(m, {10, 0, 0}).Z, Project(m, {-10, 0, 0}).Z);
}

TEST(OrbitCamera, OrbitAndZoomStayInRange) {
    OrbitCamera camera;
    camera.Orbit(0, 100000);
    EXPECT_LT(camera.Pitch, 1.5708f);
    camera.Zoom(1000);
    EXPECT_GE(camera.Distance, 0.01f);
    const float before = camera.Distance;
    camera.Zoom(-1);
    EXPECT_GT(camera.Distance, before);
}

TEST(OrbitCamera, PanMovesTargetInViewPlane) {
    OrbitCamera camera;
    camera.Pitch = 0;
    camera.Pan(10, 0, 100);
    // Dragging right moves the scene right, so the target moves toward the viewer's left (-Y).
    EXPECT_LT(camera.Target.Y, 0);
    EXPECT_NEAR(camera.Target.X, 0, 1e-4f);
    EXPECT_NEAR(camera.Target.Z, 0, 1e-4f);
}

}  // namespace
