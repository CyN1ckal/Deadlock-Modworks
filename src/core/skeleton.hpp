#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

struct Vec3 {
    float X = 0;
    float Y = 0;
    float Z = 0;
};

struct Quat {
    float X = 0;
    float Y = 0;
    float Z = 0;
    float W = 1;
};

struct BoneShape {
    std::string Name;
    std::optional<std::string> Parent;
    Vec3 Position;
    Quat Rotation{0, 0, 0, 1};
    float Scale = 1;
};

struct SkeletonAssessment {
    bool NeedsReview = false;
    std::string Detail;
    std::vector<std::string> Warnings;
};

struct BoneNameMappingResult {
    std::vector<std::pair<std::string, std::string>> Aliases;
    std::vector<std::string> Unmatched;
    std::vector<std::string> Ambiguous;
};

class SkeletonCompatibility {
public:
    static SkeletonAssessment Assess(const std::vector<BoneShape>& mod, const std::vector<BoneShape>& game,
        bool uses_external_animation_skeleton = false);
};

class BoneNameMapping {
public:
    // Maps only unique case-only changes. It never removes separators or guesses semantic renames.
    static BoneNameMappingResult Resolve(const std::vector<std::string>& old_names, const std::vector<std::string>& current_names);
};

}  // namespace skeleton_repair
