#include "library_model.hpp"

#include "strings.hpp"

#include <string>

namespace skeleton_repair {
namespace {

std::string HexPrefix(const BlobId& blob) {
    const std::string hex = blob.Hex();
    return hex.substr(0, hex.size() < 12 ? hex.size() : 12);
}

bool SameBlob(const BlobId& left, const BlobId& right) { return left.Bytes == right.Bytes; }

std::string JoinPaths(const std::vector<std::string>& paths) {
    std::string joined;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) {
            joined += ", ";
        }
        joined += paths[i];
    }
    return joined;
}

bool ContainsIgnoreCase(std::string_view text, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    if (query.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i + query.size() <= text.size(); ++i) {
        if (CompareIgnoreCase(text.substr(i, query.size()), query) == 0) {
            return true;
        }
    }
    return false;
}

const HistoryLink* LinkInto(const History& history, const BlobId& blob) {
    for (const HistoryLink& link : history.Links) {
        if (SameBlob(link.To, blob) && !SameBlob(link.From, link.To)) {
            return &link;
        }
    }
    return nullptr;
}

std::string HistoryLine(const HistoryVersion& version, std::string_view suffix) {
    std::string line = HexPrefix(version.Blob);
    const std::string paths = JoinPaths(version.Paths);
    if (!paths.empty()) {
        line += "  ";
        line += paths;
    }
    if (!suffix.empty()) {
        line += "  ";
        line += suffix;
    }
    return line;
}

}  // namespace

std::string FormatModelRow(const ModelRow& row) {
    return HexPrefix(row.Blob) + "  in " + std::to_string(row.SnapshotCount) + " packs  " + row.DisplayPath;
}

std::vector<std::string> FormatHistory(const History& history) {
    std::vector<std::string> lines;
    lines.reserve(history.Versions.size());
    for (size_t i = 0; i < history.Versions.size(); ++i) {
        const HistoryVersion& version = history.Versions[i];
        if (i == 0) {
            lines.push_back(HistoryLine(version, "imported"));
            continue;
        }
        const HistoryLink* link = LinkInto(history, version.Blob);
        if (link == nullptr) {
            lines.push_back(HistoryLine(version, {}));
            continue;
        }
        std::string suffix = link->Relation;
        if (!link->Detail.empty()) {
            if (!suffix.empty()) {
                suffix += "  ";
            }
            suffix += link->Detail;
        }
        lines.push_back(HistoryLine(version, suffix));
    }
    return lines;
}

std::string FormatUnproven(const std::vector<SlotObservation>& slots) {
    std::string text = kUnprovenSlotHeading;
    for (const SlotObservation& slot : slots) {
        text += '\n';
        text += HexPrefix(slot.Blob);
        text += "  ";
        text += slot.Path;
        if (!slot.PackLabel.empty()) {
            text += "  ";
            text += slot.PackLabel;
        }
    }
    return text;
}

std::vector<ModelRow> FilterModels(const std::vector<ModelRow>& rows, std::string_view query) {
    if (query.empty()) {
        return rows;
    }
    std::vector<ModelRow> matched;
    for (const ModelRow& row : rows) {
        if (ContainsIgnoreCase(row.DisplayPath, query)) {
            matched.push_back(row);
        }
    }
    return matched;
}

}  // namespace skeleton_repair
