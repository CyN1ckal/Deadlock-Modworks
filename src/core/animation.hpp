#pragma once

#include "migration.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace skeleton_repair {

// Rebinds an AG2 animation-skeleton override onto the installed bone order.
// Authored parent-space poses and mask weights stay; world poses are rebuilt.
class AnimationSkeletonMigration {
public:
    static MigrationEdit Migrate(std::span<const uint8_t> mod_bytes, std::string_view mod_path,
        std::span<const uint8_t> game_bytes, std::string_view game_path);
};

}  // namespace skeleton_repair
