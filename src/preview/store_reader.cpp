#include "store_reader.hpp"

#include <algorithm>
#include <exception>

namespace skeleton_repair {
namespace {

bool ClipSuffix(const std::string& path) {
    constexpr std::string_view kSuffix = ".vnmclip_c";
    return path.size() >= kSuffix.size() && path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

}  // namespace

const MembershipRow* VersionMembership(const std::vector<MembershipRow>& rows, const SnapshotId* preferred) {
    const MembershipRow* best = nullptr;
    for (const MembershipRow& row : rows) {
        if (preferred != nullptr && row.Snapshot.Value != preferred->Value) {
            continue;
        }
        if (best == nullptr || row.ObservedUtc > best->ObservedUtc
            || (row.ObservedUtc == best->ObservedUtc && row.Path < best->Path)) {
            best = &row;
        }
    }
    if (best != nullptr || preferred == nullptr) {
        return best;
    }
    return VersionMembership(rows, nullptr);
}

std::vector<std::string> ClipsUnderModel(std::string_view model_path, const std::vector<std::string>& candidates) {
    const size_t slash = model_path.rfind('/');
    const std::string folder = slash == std::string_view::npos ? std::string() : std::string(model_path.substr(0, slash + 1));
    std::vector<std::string> clips;
    for (const std::string& path : candidates) {
        if (path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && ClipSuffix(path)
            && std::find(clips.begin(), clips.end(), path) == clips.end()) {
            clips.push_back(path);
        }
    }
    return clips;
}

std::optional<std::vector<uint8_t>> ReadSnapshotThenGame(const SnapshotReader& store, const SnapshotId* snapshot,
    const std::string& path, const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& game) {
    if (snapshot != nullptr && !snapshot->Value.empty()) {
        try {
            if (std::optional<std::vector<uint8_t>> bytes = store.ReadSnapshotPath(*snapshot, path)) {
                return bytes;
            }
        } catch (const std::exception&) {
            // The snapshot names this path, but its blob is unreadable. Try the game archive.
        }
    }
    if (game) {
        try {
            return game(path);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

const SnapshotId* PreferredSnapshot(const std::optional<size_t>& selected_copy, const std::vector<MembershipRow>& memberships,
    SnapshotId& storage) {
    if (!selected_copy || *selected_copy >= memberships.size()) {
        return nullptr;
    }
    storage = memberships[*selected_copy].Snapshot;
    return &storage;
}

PreparedVersion PrepareVersion(const SnapshotReader& store, const HistoryVersion& version, const SnapshotId* preferred, const ResourceReader& read_game) {
    PreparedVersion prepared;
    const std::string hex = version.Blob.Hex();
    prepared.Label = hex.substr(0, hex.size() < 12 ? hex.size() : 12);
    prepared.Title = prepared.Label;
    try {
        const std::vector<uint8_t> bytes = store.ReadBlob(version.Blob);
        prepared.Geometry = DecodeModelGeometry(bytes);
        const std::vector<MembershipRow> rows = store.Memberships(version.Blob);
        if (const MembershipRow* membership = VersionMembership(rows, preferred)) {
            prepared.Snapshot = membership->Snapshot;
            prepared.HasSnapshot = true;
            if (!membership->Path.empty()) {
                prepared.Title = membership->Path;
            }
        } else if (!version.Paths.empty()) {
            prepared.Title = version.Paths.front();
        }
        const SnapshotId* snapshot = prepared.HasSnapshot ? &prepared.Snapshot : nullptr;
        if (prepared.Geometry) {
            if (const std::optional<std::string> skeleton = AnimationSkeletonPath(*prepared.Geometry)) {
                if (const std::optional<std::vector<uint8_t>> skeleton_bytes = ReadSnapshotThenGame(store, snapshot, *skeleton, read_game)) {
                    try {
                        prepared.Animation = DecodeAnimationSkeleton(*skeleton_bytes);
                    } catch (const std::exception& error) {
                        prepared.Error = error.what();
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        prepared.Geometry.reset();
        prepared.Error = error.what();
    } catch (...) {
        prepared.Geometry.reset();
        prepared.Error = "The model could not be read.";
    }
    return prepared;
}

VersionData SideData(const PreparedVersion& prepared) {
    VersionData data;
    data.Available = prepared.Geometry.has_value();
    data.Model = prepared.Geometry;
    data.AnimationSkeleton = prepared.Animation;
    data.Error = prepared.Error;
    data.Notes.push_back(prepared.Label);
    return data;
}

VersionData GameSide(const std::string& model_path, const std::optional<std::string>& skeleton_path, const ResourceReader& read_game) {
    VersionData data;
    std::optional<std::vector<uint8_t>> bytes;
    try {
        if (read_game) {
            bytes = read_game(model_path);
        }
    } catch (const std::exception& error) {
        data.Error = error.what();
        return data;
    }
    if (!bytes) {
        data.Notes.push_back("The game has no model at this path.");
        return data;
    }
    data.Available = true;
    data.Notes.push_back("From the game archive.");
    try {
        data.Model = DecodeModelGeometry(*bytes);
    } catch (const std::exception& error) {
        data.Error = error.what();
    }
    if (!skeleton_path || !read_game) {
        return data;
    }
    try {
        if (const std::optional<std::vector<uint8_t>> skeleton_bytes = read_game(*skeleton_path)) {
            data.AnimationSkeleton = DecodeAnimationSkeleton(*skeleton_bytes);
        }
    } catch (const std::exception& error) {
        if (!data.Error.empty()) {
            data.Error += " ";
        }
        data.Error += error.what();
    }
    return data;
}

std::vector<std::string> ClipsForPrepared(const SnapshotReader& store, const PreparedVersion& prepared, const std::vector<std::string>& game_clips) {
    std::vector<std::string> candidates;
    if (prepared.HasSnapshot) {
        try {
            candidates = store.SnapshotPaths(prepared.Snapshot, "vnmclip_c");
        } catch (const std::exception&) {
            candidates.clear();
        }
    }
    candidates.insert(candidates.end(), game_clips.begin(), game_clips.end());
    return ClipsUnderModel(prepared.Title, candidates);
}

void AppendClips(std::vector<std::string>& clips, const std::vector<std::string>& more) {
    for (const std::string& path : more) {
        if (std::find(clips.begin(), clips.end(), path) == clips.end()) {
            clips.push_back(path);
        }
    }
}

}  // namespace skeleton_repair
