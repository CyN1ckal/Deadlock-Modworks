#pragma once

#include "cancellation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace skeleton_repair {

// Bytes written so far. The total is unknown before the archive has been read, so only progress is reported.
using ExtractProgress = std::function<void(uint64_t)>;

struct ExtractLimits {
    uint64_t MaxEntryBytes = 8ull << 30;
    uint64_t MaxTotalBytes = 32ull << 30;
    size_t MaxEntries = 100000;
};

struct ExtractedMods {
    // Mod VPKs ready to import: single-file VPKs and the _dir.vpk of each chunked one, in archive order.
    std::vector<std::filesystem::path> Archives;
    // Entries that were not mod VPKs, such as readme files, images and nested archives.
    size_t SkippedEntries = 0;
};

// .zip, .7z and .rar, by extension.
bool IsModArchive(const std::filesystem::path& path);

// Extracts the .vpk files of a downloaded zip, 7z or rar archive under `destination`, which must be empty or missing.
// Nothing else in the archive is written. Each folder inside the archive becomes one numbered folder holding only
// plain file names, so an entry path cannot place a file anywhere else, and chunk files stay beside their _dir.vpk.
// Throws InvalidData for a damaged, encrypted or oversized archive, or one without a mod VPK.
ExtractedMods ExtractModArchive(const std::filesystem::path& archive, const std::filesystem::path& destination,
    CancellationToken cancellation = {}, const ExtractProgress& progress = {}, const ExtractLimits& limits = {});

}  // namespace skeleton_repair
