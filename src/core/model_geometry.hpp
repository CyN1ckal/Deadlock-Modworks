#pragma once

#include "skeleton.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

struct Vec2 {
    float X = 0;
    float Y = 0;
};

// One decoded vertex buffer. Attributes the buffer does not carry stay empty.
// Bone indices point into ModelGeometry::Bones after the mesh's remapping table is applied.
struct GeometryVertexBuffer {
    uint32_t VertexCount = 0;
    std::vector<Vec3> Positions;
    std::vector<Vec3> Normals;
    std::vector<Vec2> TexCoords;
    std::vector<std::array<uint16_t, 4>> BoneIndices;
    // Set whenever BoneIndices is. Buffers without blend weights get {1, 0, 0, 0}.
    std::vector<std::array<float, 4>> BoneWeights;
};

struct GeometryDrawCall {
    uint32_t VertexBuffer = 0;
    uint32_t IndexBuffer = 0;
    uint32_t StartIndex = 0;
    uint32_t IndexCount = 0;
    int32_t BaseVertex = 0;
    std::string Material;
};

struct GeometryMesh {
    std::string Name;
    int MeshIndex = 0;
    uint64_t LodMask = 0;
    uint64_t MeshGroupMask = 0;
    Vec3 MinBounds;
    Vec3 MaxBounds;
    std::vector<GeometryVertexBuffer> VertexBuffers;
    std::vector<std::vector<uint32_t>> IndexBuffers;
    std::vector<GeometryDrawCall> DrawCalls;

    // LOD 0 is the most detailed. A mesh with no LOD mask is drawn at every level.
    bool InLod(int level) const { return LodMask == 0 || ((LodMask >> level) & 1) != 0; }
};

struct GeometryBone {
    std::string Name;
    int Parent = -1;
    Vec3 LocalPosition;
    Quat LocalRotation;
    // Bind pose in model space.
    Vec3 ModelPosition;
    Quat ModelRotation;
};

struct ModelGeometry {
    std::vector<GeometryMesh> Meshes;
    std::vector<GeometryBone> Bones;
    // m_refMeshes entries. These live in separate .vmesh_c files and are not decoded here.
    std::vector<std::string> ExternalMeshes;
    // m_vecNmSkeletonRefs entries: the AG2 animation skeletons (.vnmskel) this model animates with.
    std::vector<std::string> AnimationSkeletons;
    uint64_t DefaultMeshGroupMask = 0;
};

// Decodes the embedded meshes and bind-pose skeleton of a compiled .vmdl_c.
// Layout pinned to ValveResourceFormat 20.0.6980 Model.GetEmbeddedMeshes and VBIB.
ModelGeometry DecodeModelGeometry(std::span<const uint8_t> file);

// Reads the bones of a compiled AG2 animation skeleton (.vnmskel_c).
// Model-space positions come from m_modelSpaceReferencePose, as stored by the compiler or the repair.
std::vector<GeometryBone> DecodeAnimationSkeleton(std::span<const uint8_t> file);

}  // namespace skeleton_repair
