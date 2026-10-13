#pragma once

#include "model_versions.hpp"

namespace skeleton_repair {

// Decodes the mod and game versions and runs ModelMigration and AnimationSkeletonMigration for the repaired one.
// The full update also fixes dependent resources and renames.
ModelVersions BuildModelVersions(const ModelSources& sources);

}  // namespace skeleton_repair
