#pragma once

#include "cancellation.hpp"
#include "content_types.hpp"
#include "repair_journal.hpp"
#include "snapshot_reader.hpp"

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

// The VPKs ImportFolder imports from a folder, ordered by name: not recursive, without chunk files and the game archive
// (a pak01_dir.vpk with pak01_000.vpk beside it). `skipped` receives the number of files left out.
std::vector<std::filesystem::path> FolderImportCandidates(const std::filesystem::path& directory, int* skipped = nullptr);

// Content-addressed library. M1 imports blobs. M2 lists them. M3 records repair edges. M4 queries history.
// M6 drops observations and collects unreferenced files.
class ContentStore final : public RepairJournal, public SnapshotReader {
public:
    static ContentStore Open(const std::filesystem::path& directory);

    ContentStore(const ContentStore&) = delete;
    ContentStore& operator=(const ContentStore&) = delete;
    ContentStore(ContentStore&&) noexcept;
    ContentStore& operator=(ContentStore&&) noexcept;
    ~ContentStore();

    SnapshotId ImportVpk(const std::filesystem::path& path, CancellationToken cancellation = {}) override;
    // Imports every eligible VPK in the folder, non-recursive. Skips chunk files and the game archive.
    // A failed archive is listed and does not roll back archives already committed.
    FolderImportResult ImportFolder(const std::filesystem::path& directory, CancellationToken cancellation = {});
    std::vector<uint8_t> ReadBlob(const BlobId& id) const override;
    // Membership bytes for a normalized path. Empty when the snapshot has no such path. Does not open a VPK.
    std::optional<std::vector<uint8_t>> ReadSnapshotPath(const SnapshotId& snapshot, std::string_view path) const override;
    // Membership paths of one kind, ordered by path. Used to find clips in a snapshot.
    std::vector<std::string> SnapshotPaths(const SnapshotId& snapshot, std::string_view kind) const override;
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
    std::vector<MembershipRow> Memberships(const BlobId& blob) const override;
    std::vector<ModelRow> ExactDuplicateModels() const;
    SnapshotId RecordRepair(const RepairRequest& repair, CancellationToken cancellation = {}) override;
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
    // The game archive, a pak01_dir.vpk with pak01_000.vpk beside it, is always refused.
    // reject_game_folder also refuses any VPK beside the game archive.
    SnapshotId ImportVpkFile(const std::filesystem::path& path, CancellationToken cancellation, bool reject_game_folder);

    std::unique_ptr<State> state_;
};

}  // namespace skeleton_repair
