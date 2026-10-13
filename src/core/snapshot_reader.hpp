#pragma once

#include "content_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Read-only access to the files of a stored snapshot. ContentStore implements it.
class SnapshotReader {
public:
    virtual ~SnapshotReader() = default;

    // Membership bytes for a normalized path. Empty when the snapshot has no such path. Does not open a VPK.
    virtual std::optional<std::vector<uint8_t>> ReadSnapshotPath(const SnapshotId& snapshot, std::string_view path) const = 0;

protected:
    SnapshotReader() = default;
    SnapshotReader(const SnapshotReader&) = default;
    SnapshotReader& operator=(const SnapshotReader&) = default;
};

}  // namespace skeleton_repair
