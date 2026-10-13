#pragma once

#include "snapshot_reader.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// The membership whose snapshot supplies textures and clips for a history version.
// A preferred snapshot wins when this blob is in it. Otherwise the latest observation wins.
const MembershipRow* VersionMembership(const std::vector<MembershipRow>& rows, const SnapshotId* preferred);

// Same folder rule as the VPK browser: a .vnmclip_c path under the model file's directory.
std::vector<std::string> ClipsUnderModel(std::string_view model_path, const std::vector<std::string>& candidates);

// Snapshot bytes first, then the live game archive. Does not open an observation path.
std::optional<std::vector<uint8_t>> ReadSnapshotThenGame(const SnapshotReader& store, const SnapshotId* snapshot,
    const std::string& path, const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& game);

}  // namespace skeleton_repair
