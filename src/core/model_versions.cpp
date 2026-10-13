#include "model_versions.hpp"

#include "vpk.hpp"

namespace skeleton_repair {

const char* ModelVersionName(ModelVersion version) {
    switch (version) {
    case ModelVersion::Mod: return "Mod (before)";
    case ModelVersion::Repaired: return "Repaired (after)";
    case ModelVersion::Game: return "Game (current)";
    }
    return "";
}

const char* ModelOutcomeName(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return "Repairable";
    case ModelOutcome::NeedsReview: return "Needs review";
    case ModelOutcome::Updated: return "Updated";
    case ModelOutcome::Current: return "Current";
    case ModelOutcome::Skipped: return "Skipped";
    case ModelOutcome::Failed: return "Failed";
    }
    return "";
}

std::optional<std::string> AnimationSkeletonPath(const ModelGeometry& model) {
    if (model.AnimationSkeletons.empty()) {
        return std::nullopt;
    }
    return VpkArchive::Normalize(model.AnimationSkeletons.front() + "_c");
}

}  // namespace skeleton_repair
