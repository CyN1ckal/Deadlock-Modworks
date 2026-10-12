#pragma once

#include "report.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// One file's repair decision. Bytes is set only when the resource should be written.
struct MigrationEdit {
    std::optional<std::vector<uint8_t>> Bytes;
    ModelOutcome Outcome = ModelOutcome::Failed;
    std::string Detail;
    // Unset when the migration did not supply a list. An empty list is still a list.
    std::optional<std::vector<std::string>> Warnings;
    // Old name, then the installed spelling. Unset when no rename pass ran.
    // An engaged empty list means the pass ran and found no case-only renames.
    std::optional<std::vector<std::pair<std::string, std::string>>> BoneAliases;
};

}  // namespace skeleton_repair
