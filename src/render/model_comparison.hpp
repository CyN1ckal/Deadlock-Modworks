#pragma once

#include "model_geometry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

enum class BoneChange {
    Same,
    // Model-space position differs by more than the tolerance.
    Moved,
    // Parent bone name differs; the position may or may not have moved.
    Reparented,
    // Only in the right-hand skeleton.
    Added,
    // Only in the left-hand skeleton.
    Removed,
};

struct BoneDiff {
    std::string Name;
    BoneChange Change = BoneChange::Same;
    float Distance = 0;
    std::string LeftParent;
    std::string RightParent;
};

// Bones are matched by exact name. Order and indices do not matter.
struct SkeletonDiff {
    // Right-hand order first, then bones only on the left.
    std::vector<BoneDiff> Bones;
    size_t Same = 0;
    size_t Moved = 0;
    size_t Reparented = 0;
    size_t Added = 0;
    size_t Removed = 0;

    bool Identical() const { return Moved == 0 && Reparented == 0 && Added == 0 && Removed == 0; }
};

SkeletonDiff CompareSkeletons(const std::vector<GeometryBone>& left, const std::vector<GeometryBone>& right, float tolerance = 0.01f);

// Compares which bones (by name) and weights drive each vertex.
// A skeleton reorder changes blend indices but must leave this unchanged.
struct BindingDiff {
    // False when the meshes differ, so vertices cannot be paired.
    bool Comparable = false;
    std::string Reason;
    uint64_t Vertices = 0;
    uint64_t Changed = 0;
    // A few examples for the report: "mesh: vertex N".
    std::vector<std::string> Examples;
};

BindingDiff CompareBindings(const ModelGeometry& left, const ModelGeometry& right);

// RGBA8 line colors per bone for a diff view. The left side marks removed bones, the right side marks
// added, moved and reparented bones; unchanged bones keep the normal skeleton color.
std::vector<uint32_t> DiffColors(const std::vector<GeometryBone>& bones, const SkeletonDiff& diff, bool left_side);

}  // namespace skeleton_repair
