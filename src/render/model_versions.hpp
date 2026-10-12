#pragma once

#include "migration.hpp"
#include "model_geometry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

enum class ModelVersion {
    Mod,
    Repaired,
    Game,
};

const char* ModelVersionName(ModelVersion version);
const char* ModelOutcomeName(ModelOutcome outcome);

// The files behind one mod model, read by the caller from the mod and game archives.
struct ModelSources {
    std::string ModelPath;
    std::vector<uint8_t> ModModel;
    std::optional<std::string> GameModelPath;
    std::optional<std::vector<uint8_t>> GameModel;
    // The .vnmskel_c the model animates with, if any.
    std::optional<std::string> SkeletonPath;
    // Set when the mod ships its own copy of the animation skeleton.
    std::optional<std::vector<uint8_t>> ModSkeleton;
    std::optional<std::vector<uint8_t>> GameSkeleton;
};

struct VersionData {
    bool Available = false;
    // How this version was produced, for the UI ("Model unchanged (Current)." and so on).
    std::vector<std::string> Notes;
    std::string Error;
    std::optional<ModelGeometry> Model;
    // The skeleton animations play on in-game: the mod's override, else the game's.
    std::optional<std::vector<GeometryBone>> AnimationSkeleton;
};

// Mod, repaired and game versions of one model. "Repaired" runs ModelMigration and
// AnimationSkeletonMigration directly; the full update also fixes dependent resources and renames.
struct ModelVersions {
    std::string ModelPath;
    std::optional<std::string> SkeletonPath;
    std::optional<MigrationEdit> ModelEdit;
    std::optional<MigrationEdit> SkeletonEdit;
    std::array<VersionData, 3> Versions;

    const VersionData& Get(ModelVersion version) const { return Versions[static_cast<size_t>(version)]; }
};

ModelVersions BuildModelVersions(const ModelSources& sources);

// "<first m_vecNmSkeletonRefs entry>_c", normalized like VPK paths, or nothing.
std::optional<std::string> AnimationSkeletonPath(const ModelGeometry& model);

}  // namespace skeleton_repair
