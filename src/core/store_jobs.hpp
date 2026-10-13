#pragma once

#include "cancellation.hpp"
#include "content_types.hpp"
#include "report.hpp"

#include <filesystem>
#include <string>

namespace skeleton_repair {

// Store workflows the version history starts on a worker thread. Each opens its own ContentStore in `store_directory`.

// Writes one model of a snapshot to `destination` as a pack, then imports that pack.
void AssembleIntoStore(const std::filesystem::path& store_directory, const SnapshotId& source, const BlobId& model,
    const std::filesystem::path& destination, CancellationToken cancellation = {});

// Repairs the VPK in place, with a backup, and records the input and the repair in the store.
UpdateReport RepairIntoStore(const std::filesystem::path& store_directory, const std::string& vpk_path,
    const std::string& game_directory, CancellationToken cancellation = {});

}  // namespace skeleton_repair
