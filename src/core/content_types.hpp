#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// The records the content store hands out. Plain values with no store behind them, so code that only formats or
// compares them does not depend on the store.

struct BlobId {
    std::array<uint8_t, 32> Bytes{};

    std::string Hex() const {  // 64 lowercase hex
        constexpr char kDigits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(Bytes.size() * 2);
        for (const uint8_t byte : Bytes) {
            hex.push_back(kDigits[byte >> 4]);
            hex.push_back(kDigits[byte & 0x0f]);
        }
        return hex;
    }
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

// One folder import. SkippedFiles counts chunk VPKs and the game archive only.
struct FolderImportResult {
    std::vector<SnapshotId> Snapshots;
    int SkippedFiles = 0;
    std::vector<FolderImportFailure> Failures;

    std::string Report() const { return "Skipped " + std::to_string(SkippedFiles) + " files."; }
};

}  // namespace skeleton_repair
