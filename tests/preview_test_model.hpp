#pragma once

#include "model_geometry.hpp"

namespace preview_test {

using skeleton_repair::GeometryBone;
using skeleton_repair::GeometryMesh;
using skeleton_repair::ModelGeometry;
using skeleton_repair::Vec3;

// A quad in the YZ plane facing +X, spanning y = -half..half and z = 0..20.
inline GeometryMesh Quad(const char* name, float half, uint64_t lod_mask, const char* material) {
    GeometryMesh mesh;
    mesh.Name = name;
    mesh.LodMask = lod_mask;
    mesh.MeshGroupMask = 1;
    mesh.MinBounds = {0, -half, 0};
    mesh.MaxBounds = {0, half, 20};
    skeleton_repair::GeometryVertexBuffer vertices;
    vertices.VertexCount = 4;
    vertices.Positions = {{0, -half, 0}, {0, half, 0}, {0, half, 20}, {0, -half, 20}};
    vertices.Normals = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    mesh.VertexBuffers.push_back(vertices);
    mesh.IndexBuffers.push_back({0, 1, 2, 0, 2, 3});
    mesh.DrawCalls.push_back({0, 0, 0, 6, 0, material});
    return mesh;
}

// LOD 0 quad, a wider LOD 1 quad, and a two-bone spine through the middle.
inline ModelGeometry TwoLodModel() {
    ModelGeometry geometry;
    geometry.DefaultMeshGroupMask = 1;
    geometry.Meshes.push_back(Quad("body", 10, 1, "models/test/body.vmat"));
    geometry.Meshes.push_back(Quad("body_lod1", 15, 2, "models/test/body_lod1.vmat"));
    GeometryBone root;
    root.Name = "root";
    root.ModelPosition = {0, 0, 0};
    GeometryBone head;
    head.Name = "head";
    head.Parent = 0;
    head.ModelPosition = {0, 0, 18};
    geometry.Bones = {root, head};
    return geometry;
}

}  // namespace preview_test
