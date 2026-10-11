#pragma once

#include "vpk.hpp"

#include <filesystem>
#include <functional>
#include <optional>

namespace skeleton_repair {

// Writes and validates a complete archive before publishing it.
class ArchiveTransaction {
public:
    // Returns the backup path when replace_source is true.
    static std::optional<std::filesystem::path> Write(
        const VpkArchive& source,
        const std::filesystem::path& source_path,
        const std::filesystem::path& destination,
        const std::vector<VpkChange>& changes,
        bool replace_source,
        CancellationToken cancellation = {},
        const std::function<void()>& before_commit = {});
};

}  // namespace skeleton_repair
