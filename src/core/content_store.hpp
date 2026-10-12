#pragma once

#include "cancellation.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skeleton_repair {

struct BlobId {
    std::array<uint8_t, 32> Bytes{};
    std::string Hex() const;  // 64 lowercase hex
};

struct SnapshotId {
    std::string Value;  // 32 lowercase hex, no dashes
};

struct ModelRow {
    BlobId Blob;
    std::string DisplayPath;  // shortest path spelling for this blob
    int SnapshotCount = 0;
    int PathCount = 0;
    std::string LineageId;
};

struct PackRow {
    SnapshotId Snapshot;
    std::string Label;  // filename of the latest observation, or the snapshot id hex if none
    int ModelCount = 0;  // memberships whose kind is vmdl_c
    int ObservationCount = 0;
    std::string ObservedUtc;
    std::string ObservationId;  // id of that latest observation, empty when the snapshot has none
    std::string ObservationPath;  // absolute path of that observation, empty when the snapshot has none
};

struct MembershipRow {
    SnapshotId Snapshot;
    std::string Path;
    std::string PackLabel;
    std::string ObservedUtc;
    std::string ObservationId;  // same latest observation as PackLabel and ObservedUtc
};

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

struct HistoryVersion {
    BlobId Blob;
    std::optional<BlobId> Data;
    std::vector<std::string> Paths;
    int SnapshotCount = 0;
    std::string FirstSeenUtc;
};

struct HistoryLink {
    BlobId From;
    BlobId To;
    std::string Relation;  // "mutated", "renamed", or "renamed+mutated"
    std::string Detail;
    std::string ToolVersion;
    std::string FinishedUtc;
};

struct History {
    std::string LineageId;
    std::vector<HistoryVersion> Versions;  // oldest first, one per blob
    std::vector<HistoryLink> Links;
};

struct SlotObservation {
    BlobId Blob;
    std::string LineageId;
    std::string Path;
    std::string PackLabel;
    std::string ObservedUtc;
};

inline constexpr const char* kUnprovenSlotHeading = "Same path, not proven related";

struct GcStats {
    int IncomingFilesRemoved = 0;
    int BlobFilesRemoved = 0;
    int BlobRowsRemoved = 0;
};

// Content-addressed library. M1 imports blobs. M2 lists them. M3 records repair edges. M4 queries history.
// M6 drops observations and collects unreferenced files.
class ContentStore {
public:
    static ContentStore Open(const std::filesystem::path& directory);

    ContentStore(const ContentStore&) = delete;
    ContentStore& operator=(const ContentStore&) = delete;
    ContentStore(ContentStore&&) noexcept;
    ContentStore& operator=(ContentStore&&) noexcept;
    ~ContentStore();

    SnapshotId ImportVpk(const std::filesystem::path& path, CancellationToken cancellation = {});
    std::vector<uint8_t> ReadBlob(const BlobId& id) const;
    // Membership bytes for a normalized path. Empty when the snapshot has no such path. Does not open a VPK.
    std::optional<std::vector<uint8_t>> ReadSnapshotPath(const SnapshotId& snapshot, std::string_view path) const;
    // Membership paths of one kind, ordered by path. Used to find clips in a snapshot.
    std::vector<std::string> SnapshotPaths(const SnapshotId& snapshot, std::string_view kind) const;
    std::vector<ModelRow> ListModels() const;
    std::vector<PackRow> ListPacks() const;
    std::vector<MembershipRow> Memberships(const BlobId& blob) const;
    std::vector<ModelRow> ExactDuplicateModels() const;
    SnapshotId RecordRepair(const RepairRequest& repair, CancellationToken cancellation = {});
    History HistoryOf(const BlobId& blob) const;
    std::vector<SlotObservation> UnprovenSamePath(const BlobId& blob) const;
    void DropObservation(std::string_view observation_id);
    GcStats CollectGarbage();

private:
    struct State;
    explicit ContentStore(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

}  // namespace skeleton_repair
