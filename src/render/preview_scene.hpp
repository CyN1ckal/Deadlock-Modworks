#pragma once

#include "model_geometry.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Column-major, as GLSL expects.
using Mat4 = std::array<float, 16>;

// RGBA8 with red in the low byte, matching VK_FORMAT_R8G8B8A8_UNORM vertex attributes.
constexpr uint32_t PackRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return static_cast<uint32_t>(r) | static_cast<uint32_t>(g) << 8 | static_cast<uint32_t>(b) << 16 | static_cast<uint32_t>(a) << 24;
}

constexpr uint32_t kBoneLineColor = PackRgba(255, 200, 40);
constexpr uint32_t kAddedBoneColor = PackRgba(60, 200, 255);
constexpr uint32_t kMovedBoneColor = PackRgba(255, 60, 50);
constexpr uint32_t kReparentedBoneColor = PackRgba(255, 130, 20);
constexpr uint32_t kRemovedBoneColor = PackRgba(235, 80, 235);
constexpr uint32_t kUnskinnedColor = PackRgba(120, 120, 120);

// Stable color per bone name, so the same bone has the same color in every model and skeleton order.
uint32_t BoneNameColor(std::string_view name);

struct PreviewVertex {
    Vec3 Position;
    Vec3 Normal;
    // Color of the bone with the largest weight, or kUnskinnedColor.
    uint32_t BoneColor = kUnskinnedColor;
    Vec2 TexCoord;
    // Model skeleton bones and weights (out of 255). All-zero weights mean the vertex is not skinned.
    std::array<uint16_t, 4> BoneIndices{};
    std::array<uint8_t, 4> BoneWeights{};
};
static_assert(sizeof(PreviewVertex) == 48);

struct LineVertex {
    Vec3 Position;
    // RGBA8, red in the low byte.
    uint32_t Color = 0;
};
static_assert(sizeof(LineVertex) == 16);

struct PreviewDraw {
    uint32_t FirstIndex = 0;
    uint32_t IndexCount = 0;
    int32_t VertexOffset = 0;
    uint32_t Mesh = 0;
    // Linear RGB used when coloring by material.
    std::array<float, 3> MaterialColor{};
    // Material resource name from the draw call (".vmat", no "_c").
    std::string Material;
};

struct PreviewMesh {
    std::string Name;
    uint64_t LodMask = 0;
    uint64_t MeshGroupMask = 0;
    uint32_t VertexCount = 0;
    uint32_t TriangleCount = 0;
    Vec3 Min;
    Vec3 Max;

    bool InLod(int level) const { return LodMask == 0 || ((LodMask >> level) & 1) != 0; }
};

// Everything the renderer uploads, flattened into one vertex and one index buffer.
struct PreviewScene {
    std::vector<PreviewVertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<PreviewDraw> Draws;
    std::vector<PreviewMesh> Meshes;
    std::vector<LineVertex> Skeleton;
    std::vector<LineVertex> Grid;
    size_t BoneCount = 0;
    int LodCount = 1;
    uint64_t DefaultMeshGroupMask = 0;
    // Bounds of the meshes shown by default, or of the skeleton when there are none.
    Vec3 Min;
    Vec3 Max;
};

PreviewScene BuildPreviewScene(const ModelGeometry& geometry);

// One line from each bone to its parent. Colors are per bone; missing entries use kBoneLineColor.
std::vector<LineVertex> SkeletonLines(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors = {});

// One byte per mesh: in the LOD and in the model's default mesh group.
std::vector<uint8_t> DefaultVisibility(const PreviewScene& scene, int lod);

// Orbits a target with Z up, matching Source 2 model space.
struct OrbitCamera {
    Vec3 Target;
    // Radians. Yaw 0 puts the eye on +X looking back toward -X, the front of a hero model.
    float Yaw = 0;
    float Pitch = 0.15f;
    float Distance = 100;
    float FovY = 0.75f;

    static OrbitCamera Framing(const Vec3& min, const Vec3& max);

    Vec3 Eye() const;
    // Vulkan clip space: Y down, depth 0..1.
    Mat4 ViewProjection(float aspect) const;

    void Orbit(float dx_pixels, float dy_pixels);
    void Pan(float dx_pixels, float dy_pixels, float viewport_height);
    void Zoom(float wheel_steps);
};

}  // namespace skeleton_repair
