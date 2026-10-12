#pragma once

#include "migration.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace skeleton_repair {

// Repairs an embedded or legacy render skeleton. AG2 models that point at
// m_vecNmSkeletonRefs are left unchanged.
class ModelMigration {
public:
    static MigrationEdit Migrate(std::span<const uint8_t> mod_bytes, std::string_view mod_path,
        std::span<const uint8_t> game_bytes, std::string_view game_path);
};

}  // namespace skeleton_repair
