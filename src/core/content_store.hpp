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
    std::string Label;  // see ContentStore::SnapshotLabel
    int ModelCount = 0;  // memberships whose kind is vmdl_c
    int ObservationCount = 0;
    std::string ObservedUtc;
    std::string ObservationId;  // id of that latest observation, empty when the snapshot has none
    std::string ObservationPath;  // absolute path of that observation, empty when the snapshot has none
};

// One file of a snapshot, as stored.
struct SnapshotMember {
    std::string Path;
    BlobId Blob;
    uint32_t Crc32 = 0;
    int64_t Size = 0;
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

struct FolderImportFailure {
    std::filesystem::path Path;
    std::string Message;
};

// One folder import. SkippedFiles counts chunk VPKs and pak01_dir.vpk only.
struct FolderImportResult {
    std::vector<SnapshotId> Snapshots;
    int SkippedFiles = 0;
    std::vector<FolderImportFailure> Failures;

    std::string Report() const { return "Skipped " + std::to_string(SkippedFiles) + " files."; }
};

// The VPKs ImportFolder imports from a folder, ordered by name: not recursive, without chunk files and pak01_dir.vpk.
// `skipped` receives the number of chunk files and pak01_dir.vpk.
std::vector<std::filesystem::path> FolderImportCandidates(const std::filesystem::path& directory, int* skipped = nullptr);

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
    // Imports every eligible VPK in the folder, non-recursive. Skips chunk files and pak01_dir.vpk.
    // A failed archive is listed and does not roll back archives already committed.
    FolderImportResult ImportFolder(const std::filesystem::path& directory, CancellationToken cancellation = {});
    std::vector<uint8_t> ReadBlob(const BlobId& id) const;
    // Membership bytes for a normalized path. Empty when the snapshot has no such path. Does not open a VPK.
    std::optional<std::vector<uint8_t>> ReadSnapshotPath(const SnapshotId& snapshot, std::string_view path) const;
    // Membership paths of one kind, ordered by path. Used to find clips in a snapshot.
    std::vector<std::string> SnapshotPaths(const SnapshotId& snapshot, std::string_view kind) const;
    // Every file of the snapshot, ordered by path. Empty when the snapshot does not exist.
    std::vector<SnapshotMember> SnapshotMembers(const SnapshotId& snapshot) const;
    // Lowercase hex of the snapshot's manifest SHA-256. The same files give the same id in every library.
    std::optional<std::string> SnapshotContentId(const SnapshotId& snapshot) const;
    std::optional<SnapshotId> FindSnapshot(std::string_view content_id) const;
    // The name a snapshot is shown under: the name it was given, else the latest file name it was imported under
    // without its extension, skipping generic pakNN_dir.vpk names, else the models it contains, else its id.
    // Empty when the snapshot does not exist.
    std::string SnapshotLabel(const SnapshotId& snapshot) const;
    // Only the name given with RenameSnapshot, or inherited by a repaired version.
    std::optional<std::string> SnapshotName(const SnapshotId& snapshot) const;
    // Names the snapshot, trimmed. An empty name removes it, so the label is derived again.
    void RenameSnapshot(const SnapshotId& snapshot, std::string_view name);
    std::vector<ModelRow> ListModels() const;
    std::vector<PackRow> ListPacks() const;
    std::vector<MembershipRow> Memberships(const BlobId& blob) const;
    std::vector<ModelRow> ExactDuplicateModels() const;
    SnapshotId RecordRepair(const RepairRequest& repair, CancellationToken cancellation = {});
    History HistoryOf(const BlobId& blob) const;
    std::vector<SlotObservation> UnprovenSamePath(const BlobId& blob) const;
    void DropObservation(std::string_view observation_id);
    GcStats CollectGarbage();
    // Writes one embedded VPK of the selected model blobs and their resolved snapshot refs.
    // Unresolved refs are omitted. The written file is not imported.
    void Assemble(const SnapshotId& snapshot, const std::vector<BlobId>& models,
        const std::filesystem::path& destination, CancellationToken cancellation = {});

private:
    struct State;
    explicit ContentStore(std::unique_ptr<State> state);
    // reject_game_folder refuses a VPK whose parent contains pak01_dir.vpk.
    // The file named pak01_dir.vpk is always refused.
    SnapshotId ImportVpkFile(const std::filesystem::path& path, CancellationToken cancellation, bool reject_game_folder);

    std::unique_ptr<State> state_;
};

}  // namespace skeleton_repair
