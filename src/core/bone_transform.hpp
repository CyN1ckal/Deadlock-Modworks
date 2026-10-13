#pragma once

#include "skeleton.hpp"

namespace skeleton_repair {

// A bone transform relative to its parent.
struct BoneTransform {
    Vec3 Translation;
    Quat Rotation;
    float Scale = 1;
};

}  // namespace skeleton_repair
