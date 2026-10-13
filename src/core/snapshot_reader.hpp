#pragma once

#include "content_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Read-only access to stored blobs and snapshots, for code that shows them. ContentStore implements it.
class SnapshotReader {
public:
    virtual ~SnapshotReader() = default;

    virtual std::vector<uint8_t> ReadBlob(const BlobId& id) const = 0;
    // Membership bytes for a normalized path. Empty when the snapshot has no such path. Does not open a VPK.
    virtual std::optional<std::vector<uint8_t>> ReadSnapshotPath(const SnapshotId& snapshot, std::string_view path) const = 0;
    // Membership paths of one kind, ordered by path. Used to find clips in a snapshot.
    virtual std::vector<std::string> SnapshotPaths(const SnapshotId& snapshot, std::string_view kind) const = 0;
    virtual std::vector<MembershipRow> Memberships(const BlobId& blob) const = 0;

protected:
    SnapshotReader() = default;
    SnapshotReader(const SnapshotReader&) = default;
    SnapshotReader& operator=(const SnapshotReader&) = default;
};

}  // namespace skeleton_repair
