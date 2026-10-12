#include "model_versions.hpp"

#include "animation.hpp"
#include "model.hpp"
#include "vpk.hpp"

#include <exception>
#include <utility>

namespace skeleton_repair {
namespace {

template <typename T, typename Fn>
std::optional<T> TryDecode(Fn&& decode, std::string& error, const std::string& what) {
    try {
        return decode();
    } catch (const std::exception& ex) {
        if (!error.empty()) {
            error += " ";
        }
        error += what + ": " + ex.what();
        return std::nullopt;
    }
}

std::optional<ModelGeometry> DecodeModel(const std::vector<uint8_t>& bytes, std::string& error, const std::string& what) {
    return TryDecode<ModelGeometry>([&] { return DecodeModelGeometry(bytes); }, error, what);
}

std::optional<std::vector<GeometryBone>> DecodeSkeleton(const std::vector<uint8_t>& bytes, std::string& error, const std::string& what) {
    return TryDecode<std::vector<GeometryBone>>([&] { return DecodeAnimationSkeleton(bytes); }, error, what);
}

std::string Describe(const MigrationEdit& edit) {
    return std::string(ModelOutcomeName(edit.Outcome)) + (edit.Detail.empty() ? "" : ": " + edit.Detail);
}

}  // namespace

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

ModelVersions BuildModelVersions(const ModelSources& sources) {
    ModelVersions result;
    result.ModelPath = sources.ModelPath;
    result.SkeletonPath = sources.SkeletonPath;

    VersionData& mod = result.Versions[static_cast<size_t>(ModelVersion::Mod)];
    VersionData& repaired = result.Versions[static_cast<size_t>(ModelVersion::Repaired)];
    VersionData& game = result.Versions[static_cast<size_t>(ModelVersion::Game)];

    // Game: what the installed game ships.
    std::optional<std::vector<GeometryBone>> game_skeleton;
    if (sources.GameSkeleton) {
        game_skeleton = DecodeSkeleton(*sources.GameSkeleton, game.Error, "Game animation skeleton");
    }
    if (sources.GameModel) {
        game.Available = true;
        game.Model = DecodeModel(*sources.GameModel, game.Error, "Game model");
        game.AnimationSkeleton = game_skeleton;
        if (sources.GameModelPath) {
            game.Notes.push_back("From the game: " + *sources.GameModelPath + ".");
        }
    } else {
        game.Notes.push_back("The game has no model at this path.");
    }

    // Mod: the archive as it is now.
    mod.Available = true;
    mod.Model = DecodeModel(sources.ModModel, mod.Error, "Mod model");
    if (sources.ModSkeleton) {
        mod.AnimationSkeleton = DecodeSkeleton(*sources.ModSkeleton, mod.Error, "Mod animation skeleton");
        mod.Notes.push_back("Animation skeleton: the mod's own copy.");
    } else if (game_skeleton) {
        mod.AnimationSkeleton = game_skeleton;
        mod.Notes.push_back("Animation skeleton: the mod does not override it, so the game's is used.");
    }

    // Repaired: the two migrations the updater runs for this model.
    if (sources.GameModel && sources.GameModelPath) {
        result.ModelEdit = ModelMigration::Migrate(sources.ModModel, sources.ModelPath, *sources.GameModel, *sources.GameModelPath);
    }
    if (sources.ModSkeleton && sources.GameSkeleton && sources.SkeletonPath) {
        result.SkeletonEdit = AnimationSkeletonMigration::Migrate(*sources.ModSkeleton, *sources.SkeletonPath, *sources.GameSkeleton,
            *sources.SkeletonPath);
    }
    repaired.Available = result.ModelEdit.has_value() || result.SkeletonEdit.has_value();
    if (!repaired.Available) {
        repaired.Notes.push_back("Nothing to repair against: the game has neither this model nor its animation skeleton.");
        return result;
    }

    if (result.ModelEdit && result.ModelEdit->Bytes) {
        repaired.Model = DecodeModel(*result.ModelEdit->Bytes, repaired.Error, "Repaired model");
        repaired.Notes.push_back("Model rewritten. " + Describe(*result.ModelEdit));
    } else {
        repaired.Model = mod.Model;
        repaired.Notes.push_back("Model unchanged. " + (result.ModelEdit ? Describe(*result.ModelEdit) : std::string("No game model to compare.")));
    }
    if (result.SkeletonEdit && result.SkeletonEdit->Bytes) {
        repaired.AnimationSkeleton = DecodeSkeleton(*result.SkeletonEdit->Bytes, repaired.Error, "Repaired animation skeleton");
        repaired.Notes.push_back("Animation skeleton rewritten. " + Describe(*result.SkeletonEdit));
    } else {
        repaired.AnimationSkeleton = mod.AnimationSkeleton;
        if (result.SkeletonEdit) {
            repaired.Notes.push_back("Animation skeleton unchanged. " + Describe(*result.SkeletonEdit));
        }
    }
    return result;
}

}  // namespace skeleton_repair
