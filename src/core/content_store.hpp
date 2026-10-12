#pragma once

#include "cancellation.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
};

struct MembershipRow {
    SnapshotId Snapshot;
    std::string Path;
    std::string PackLabel;
    std::string ObservedUtc;
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

// Content-addressed library. M1 imports blobs. M2 lists models, packs, and memberships.
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
    std::vector<ModelRow> ListModels() const;
    std::vector<PackRow> ListPacks() const;
    std::vector<MembershipRow> Memberships(const BlobId& blob) const;
    std::vector<ModelRow> ExactDuplicateModels() const;

private:
    struct State;
    explicit ContentStore(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

}  // namespace skeleton_repair
