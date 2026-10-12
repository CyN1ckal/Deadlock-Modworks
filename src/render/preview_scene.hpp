#pragma once

#include "model_geometry.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

// Column-major, as GLSL expects.
using Mat4 = std::array<float, 16>;

struct PreviewVertex {
    Vec3 Position;
    Vec3 Normal;
};
static_assert(sizeof(PreviewVertex) == 24);

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
