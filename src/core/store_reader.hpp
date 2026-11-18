#pragma once

#include "model_geometry.hpp"
#include "model_versions.hpp"
#include "resource_reader.hpp"
#include "snapshot_reader.hpp"

#include <cstddef>
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

// One stored history version, decoded for display.
struct PreparedVersion {
    std::string Label;
    std::string Title;
    std::optional<ModelGeometry> Geometry;
    std::optional<std::vector<GeometryBone>> Animation;
    SnapshotId Snapshot;
    bool HasSnapshot = false;
    std::string Error;
};

// The membership row a version should be shown from, when the user picked one copy of it.
const SnapshotId* PreferredSnapshot(const std::optional<size_t>& selected_copy, const std::vector<MembershipRow>& memberships,
    SnapshotId& storage);

// Decodes a stored version and its animation skeleton. Errors are kept in Error, never thrown.
PreparedVersion PrepareVersion(const SnapshotReader& store, const HistoryVersion& version, const SnapshotId* preferred,
    const ResourceReader& read_game);

// The comparison side for a prepared version.
VersionData SideData(const PreparedVersion& prepared);

// The game's copy of a model and its animation skeleton, as a comparison side.
VersionData GameSide(const std::string& model_path, const std::optional<std::string>& skeleton_path, const ResourceReader& read_game);

// The clips under the prepared model's folder, from its snapshot and then the game.
std::vector<std::string> ClipsForPrepared(const SnapshotReader& store, const PreparedVersion& prepared, const std::vector<std::string>& game_clips);

// Appends the paths not already listed.
void AppendClips(std::vector<std::string>& clips, const std::vector<std::string>& more);

}  // namespace skeleton_repair
