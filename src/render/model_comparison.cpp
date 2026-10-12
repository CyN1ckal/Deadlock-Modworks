#include "model_comparison.hpp"

#include "preview_scene.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace skeleton_repair {
namespace {

std::string ParentName(const std::vector<GeometryBone>& bones, const GeometryBone& bone) {
    if (bone.Parent < 0 || static_cast<size_t>(bone.Parent) >= bones.size()) {
        return {};
    }
    return bones[static_cast<size_t>(bone.Parent)].Name;
}

float Distance(const Vec3& a, const Vec3& b) {
    const float x = a.X - b.X;
    const float y = a.Y - b.Y;
    const float z = a.Z - b.Z;
    return std::sqrt(x * x + y * y + z * z);
}

std::unordered_map<std::string, size_t> IndexByName(const std::vector<GeometryBone>& bones) {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < bones.size(); ++i) {
        index.emplace(bones[i].Name, i);
    }
    return index;
}

struct Influence {
    const std::string* Bone;
    float Weight;
};

std::vector<Influence> Influences(const ModelGeometry& geometry, const GeometryVertexBuffer& buffer, size_t vertex) {
    static const std::string invalid = "<invalid>";
    std::vector<Influence> influences;
    for (size_t slot = 0; slot < 4; ++slot) {
        const float weight = buffer.BoneWeights[vertex][slot];
        if (weight <= 0) {
            continue;
        }
        const size_t bone = buffer.BoneIndices[vertex][slot];
        influences.push_back({bone < geometry.Bones.size() ? &geometry.Bones[bone].Name : &invalid, weight});
    }
    std::sort(influences.begin(), influences.end(), [](const Influence& a, const Influence& b) { return *a.Bone < *b.Bone; });
    return influences;
}

bool SameInfluences(const std::vector<Influence>& a, const std::vector<Influence>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (*a[i].Bone != *b[i].Bone || std::abs(a[i].Weight - b[i].Weight) > 1.0f / 255.0f) {
            return false;
        }
    }
    return true;
}

// Paired only when every mesh has the same name, layout and vertex positions.
std::string WhyNotComparable(const ModelGeometry& left, const ModelGeometry& right) {
    if (left.Meshes.size() != right.Meshes.size()) {
        return "The models have different meshes.";
    }
    for (size_t m = 0; m < left.Meshes.size(); ++m) {
        const GeometryMesh& a = left.Meshes[m];
        const GeometryMesh& b = right.Meshes[m];
        if (a.Name != b.Name || a.VertexBuffers.size() != b.VertexBuffers.size()) {
            return "The models have different meshes.";
        }
        for (size_t v = 0; v < a.VertexBuffers.size(); ++v) {
            const auto& pa = a.VertexBuffers[v].Positions;
            const auto& pb = b.VertexBuffers[v].Positions;
            if (pa.size() != pb.size()) {
                return "Mesh " + a.Name + " has a different vertex count.";
            }
            for (size_t i = 0; i < pa.size(); ++i) {
                if (Distance(pa[i], pb[i]) > 1e-4f) {
                    return "Mesh " + a.Name + " has different vertex positions.";
                }
            }
        }
    }
    return {};
}

}  // namespace

SkeletonDiff CompareSkeletons(const std::vector<GeometryBone>& left, const std::vector<GeometryBone>& right, float tolerance) {
    SkeletonDiff diff;
    const auto left_index = IndexByName(left);
    const auto right_index = IndexByName(right);

    for (const GeometryBone& bone : right) {
        BoneDiff entry;
        entry.Name = bone.Name;
        entry.RightParent = ParentName(right, bone);
        const auto found = left_index.find(bone.Name);
        if (found == left_index.end()) {
            entry.Change = BoneChange::Added;
            ++diff.Added;
        } else {
            const GeometryBone& before = left[found->second];
            entry.LeftParent = ParentName(left, before);
            entry.Distance = Distance(before.ModelPosition, bone.ModelPosition);
            if (entry.LeftParent != entry.RightParent) {
                entry.Change = BoneChange::Reparented;
                ++diff.Reparented;
            } else if (entry.Distance > tolerance) {
                entry.Change = BoneChange::Moved;
                ++diff.Moved;
            } else {
                ++diff.Same;
            }
        }
        diff.Bones.push_back(std::move(entry));
    }
    for (const GeometryBone& bone : left) {
        if (right_index.contains(bone.Name)) {
            continue;
        }
        BoneDiff entry;
        entry.Name = bone.Name;
        entry.Change = BoneChange::Removed;
        entry.LeftParent = ParentName(left, bone);
        diff.Bones.push_back(std::move(entry));
        ++diff.Removed;
    }
    return diff;
}

BindingDiff CompareBindings(const ModelGeometry& left, const ModelGeometry& right) {
    BindingDiff diff;
    diff.Reason = WhyNotComparable(left, right);
    if (!diff.Reason.empty()) {
        return diff;
    }
    diff.Comparable = true;
    for (size_t m = 0; m < left.Meshes.size(); ++m) {
        const GeometryMesh& mesh = left.Meshes[m];
        for (size_t v = 0; v < mesh.VertexBuffers.size(); ++v) {
            const GeometryVertexBuffer& a = mesh.VertexBuffers[v];
            const GeometryVertexBuffer& b = right.Meshes[m].VertexBuffers[v];
            const bool a_skinned = !a.BoneIndices.empty();
            const bool b_skinned = !b.BoneIndices.empty();
            for (size_t i = 0; i < a.VertexCount; ++i) {
                ++diff.Vertices;
                const bool same = a_skinned == b_skinned
                    && (!a_skinned || SameInfluences(Influences(left, a, i), Influences(right, b, i)));
                if (!same) {
                    ++diff.Changed;
                    if (diff.Examples.size() < 5) {
                        diff.Examples.push_back(mesh.Name + ": vertex " + std::to_string(i));
                    }
                }
            }
        }
    }
    return diff;
}

std::vector<uint32_t> DiffColors(const std::vector<GeometryBone>& bones, const SkeletonDiff& diff, bool left_side) {
    std::unordered_map<std::string, BoneChange> changes;
    for (const BoneDiff& entry : diff.Bones) {
        changes.emplace(entry.Name, entry.Change);
    }
    std::vector<uint32_t> colors;
    colors.reserve(bones.size());
    for (const GeometryBone& bone : bones) {
        const auto found = changes.find(bone.Name);
        const BoneChange change = found == changes.end() ? BoneChange::Same : found->second;
        uint32_t color = kBoneLineColor;
        if (left_side) {
            if (change == BoneChange::Removed) {
                color = kRemovedBoneColor;
            }
        } else if (change == BoneChange::Added) {
            color = kAddedBoneColor;
        } else if (change == BoneChange::Moved) {
            color = kMovedBoneColor;
        } else if (change == BoneChange::Reparented) {
            color = kReparentedBoneColor;
        }
        colors.push_back(color);
    }
    return colors;
}

}  // namespace skeleton_repair
