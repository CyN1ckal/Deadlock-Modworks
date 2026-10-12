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
    std::vector<std::string> Warnings;
    // Old name, then the installed spelling. Empty when no case-only rename was applied.
    std::vector<std::pair<std::string, std::string>> BoneAliases;
};

}  // namespace skeleton_repair
