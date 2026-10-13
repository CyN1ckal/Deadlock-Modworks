#include "store_reader.hpp"

#include <algorithm>

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

}  // namespace skeleton_repair
