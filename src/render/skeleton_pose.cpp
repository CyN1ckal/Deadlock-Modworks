#include "skeleton_pose.hpp"

#include <functional>
#include <unordered_map>

namespace skeleton_repair {
namespace {

Quat Multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z,
    };
}

Quat Conjugate(const Quat& q) {
    return Quat{-q.X, -q.Y, -q.Z, q.W};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.X, q.Y, q.Z};
    const Vec3 t{2 * (u.Y * v.Z - u.Z * v.Y), 2 * (u.Z * v.X - u.X * v.Z), 2 * (u.X * v.Y - u.Y * v.X)};
    return Vec3{
        v.X + q.W * t.X + (u.Y * t.Z - u.Z * t.Y),
        v.Y + q.W * t.Y + (u.Z * t.X - u.X * t.Z),
        v.Z + q.W * t.Z + (u.X * t.Y - u.Y * t.X),
    };
}

// Parent then child: scale and rotate the child's offset by the parent.
BoneTransform Compose(const BoneTransform& parent, const BoneTransform& child) {
    const Vec3 offset = Rotate(parent.Rotation, Vec3{child.Translation.X * parent.Scale, child.Translation.Y * parent.Scale,
                                                    child.Translation.Z * parent.Scale});
    return BoneTransform{Vec3{parent.Translation.X + offset.X, parent.Translation.Y + offset.Y, parent.Translation.Z + offset.Z},
        Multiply(parent.Rotation, child.Rotation), parent.Scale * child.Scale};
}

BoneTransform Inverse(const BoneTransform& transform) {
    const Quat rotation = Conjugate(transform.Rotation);
    const float scale = transform.Scale != 0 ? 1 / transform.Scale : 0;
    const Vec3 moved = Rotate(rotation, transform.Translation);
    return BoneTransform{Vec3{-moved.X * scale, -moved.Y * scale, -moved.Z * scale}, rotation, scale};
}

Mat34 ToMatrix(const BoneTransform& t) {
    const Quat& q = t.Rotation;
    const float s = t.Scale;
    const float xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
    const float xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
    const float wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;
    return Mat34{
        (1 - 2 * (yy + zz)) * s, 2 * (xy - wz) * s, 2 * (xz + wy) * s, t.Translation.X,
        2 * (xy + wz) * s, (1 - 2 * (xx + zz)) * s, 2 * (yz - wx) * s, t.Translation.Y,
        2 * (xz - wy) * s, 2 * (yz + wx) * s, (1 - 2 * (xx + yy)) * s, t.Translation.Z,
    };
}

BoneTransform Local(const GeometryBone& bone) {
    return BoneTransform{bone.LocalPosition, bone.LocalRotation, 1};
}

BoneTransform Model(const GeometryBone& bone) {
    return BoneTransform{bone.ModelPosition, bone.ModelRotation, 1};
}

// World transforms for a skeleton given each bone's parent-space transform, in any parent order.
std::vector<BoneTransform> Resolve(const std::vector<GeometryBone>& bones, const std::function<BoneTransform(size_t)>& local) {
    std::vector<BoneTransform> world(bones.size());
    std::vector<uint8_t> done(bones.size(), 0);
    std::function<const BoneTransform&(size_t)> resolve = [&](size_t index) -> const BoneTransform& {
        if (done[index] == 0) {
            // Mark first so a malformed cycle ends at the bone's own local transform.
            done[index] = 1;
            const int parent = bones[index].Parent;
            world[index] = parent >= 0 && static_cast<size_t>(parent) < bones.size() && done[static_cast<size_t>(parent)] != 1
                ? Compose(resolve(static_cast<size_t>(parent)), local(index))
                : local(index);
            done[index] = 2;
        }
        return world[index];
    };
    for (size_t i = 0; i < bones.size(); ++i) {
        resolve(i);
    }
    return world;
}

}  // namespace

SkeletonPoser::SkeletonPoser(const std::vector<GeometryBone>& render, const std::vector<GeometryBone>& animation)
    : render_(render), animation_(animation), source_(render.size(), -1) {
    std::unordered_map<std::string, int> index;
    for (size_t i = 0; i < animation_.size(); ++i) {
        index.emplace(animation_[i].Name, static_cast<int>(i));
    }
    for (size_t i = 0; i < render_.size(); ++i) {
        if (const auto found = index.find(render_[i].Name); found != index.end()) {
            source_[i] = found->second;
            ++matched_;
        }
    }
}

std::vector<BoneTransform> SkeletonPoser::ReferencePose() const {
    std::vector<BoneTransform> pose;
    pose.reserve(animation_.size());
    for (const GeometryBone& bone : animation_) {
        pose.push_back(Local(bone));
    }
    return pose;
}

PosedSkeleton SkeletonPoser::Pose(std::span<const BoneTransform> local) const {
    const std::vector<BoneTransform> animated = Resolve(animation_, [&](size_t i) { return i < local.size() ? local[i] : Local(animation_[i]); });

    // Driven bones take the animation's model-space transform; the others follow their parent at their bind offset.
    std::vector<BoneTransform> world(render_.size());
    std::vector<uint8_t> done(render_.size(), 0);
    std::function<const BoneTransform&(size_t)> resolve = [&](size_t index) -> const BoneTransform& {
        if (done[index] == 0) {
            done[index] = 1;
            const int parent = render_[index].Parent;
            if (source_[index] >= 0) {
                world[index] = animated[static_cast<size_t>(source_[index])];
            } else if (parent >= 0 && static_cast<size_t>(parent) < render_.size() && done[static_cast<size_t>(parent)] != 1) {
                world[index] = Compose(resolve(static_cast<size_t>(parent)), Local(render_[index]));
            } else {
                world[index] = Local(render_[index]);
            }
            done[index] = 2;
        }
        return world[index];
    };

    PosedSkeleton result;
    result.Skinning.reserve(render_.size());
    result.Bones = render_;
    for (size_t i = 0; i < render_.size(); ++i) {
        const BoneTransform& pose = resolve(i);
        result.Skinning.push_back(ToMatrix(Compose(pose, Inverse(Model(render_[i])))));
        result.Bones[i].ModelPosition = pose.Translation;
        result.Bones[i].ModelRotation = pose.Rotation;
    }
    result.AnimationBones = animation_;
    for (size_t i = 0; i < animation_.size(); ++i) {
        result.AnimationBones[i].ModelPosition = animated[i].Translation;
        result.AnimationBones[i].ModelRotation = animated[i].Rotation;
    }
    return result;
}

}  // namespace skeleton_repair
