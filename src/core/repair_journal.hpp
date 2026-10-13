#pragma once

#include "cancellation.hpp"
#include "content_types.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// One changed output entry. An empty output path and no bytes removes InputPath.
// Any other change writes Bytes at OutputPath. Omitted input paths are copies.
struct RepairChange {
    std::string InputPath;
    std::string OutputPath;
    std::optional<std::vector<uint8_t>> Bytes;
};

struct RepairModelNote {
    std::string InputPath;
    std::string Outcome;
    std::string Detail;
    std::vector<std::string> Warnings;
    std::vector<std::pair<std::string, std::string>> BoneRenames;
    std::string GamePath;
    std::optional<std::array<uint8_t, 32>> GameSha256;
};

struct RepairRequest {
    SnapshotId Input;
    std::vector<RepairChange> Changes;
    std::vector<RepairModelNote> Models;
    std::string OutputPath;
    std::optional<std::string> BackupPath;
};

// Where a repair is recorded: the archive it started from and the edits it made.
// The updater writes to this, and ContentStore implements it, so the updater does not depend on the store.
class RepairJournal {
public:
    virtual ~RepairJournal() = default;

    virtual SnapshotId ImportVpk(const std::filesystem::path& path, CancellationToken cancellation = {}) = 0;
    virtual SnapshotId RecordRepair(const RepairRequest& repair, CancellationToken cancellation = {}) = 0;

protected:
    RepairJournal() = default;
    RepairJournal(const RepairJournal&) = default;
    RepairJournal& operator=(const RepairJournal&) = default;
};

}  // namespace skeleton_repair
