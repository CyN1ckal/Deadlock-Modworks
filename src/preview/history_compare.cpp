#include "history_compare.hpp"

#include "error.hpp"
#include "startup.hpp"
#include "strings.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <utility>

namespace skeleton_repair {
namespace {

bool SameBlob(const BlobId& left, const BlobId& right) { return left.Bytes == right.Bytes; }

std::string Prefix(const BlobId& blob) {
    const std::string hex = blob.Hex();
    return hex.substr(0, hex.size() < 12 ? hex.size() : 12);
}

bool LinksPair(const HistoryLink& link, const BlobId& first, const BlobId& second) {
    if (SameBlob(first, second) || SameBlob(link.From, link.To)) {
        return false;
    }
    return (SameBlob(link.From, first) && SameBlob(link.To, second)) || (SameBlob(link.From, second) && SameBlob(link.To, first));
}

}  // namespace

const HistoryLink* DirectHistoryLink(const History& history, const BlobId& first, const BlobId& second) {
    for (const HistoryLink& link : history.Links) {
        if (LinksPair(link, first, second)) {
            return &link;
        }
    }
    return nullptr;
}

std::string HistoryCompareCaption(const History& history, const BlobId& first, const BlobId& second) {
    const HistoryLink* link = DirectHistoryLink(history, first, second);
    if (link == nullptr) {
        return kNoRecordedTransform;
    }
    return Prefix(link->From) + "  " + link->Relation + "  " + Prefix(link->To);
}

RepairGate RepairGateFor(bool observation_file_exists, bool game_folder_set) {
    RepairGate gate;
    if (!observation_file_exists) {
        gate.Message = kRepairSourceMissing;
        return gate;
    }
    if (!game_folder_set) {
        gate.Message = kRepairNeedsGameFolder;
        return gate;
    }
    gate.Enabled = true;
    return gate;
}

bool ObservationFileExists(const std::string& absolute_path) {
    if (absolute_path.empty()) {
        return false;
    }
    try {
        std::error_code error;
        const std::filesystem::path path(Widen(absolute_path));
        return std::filesystem::is_regular_file(path, error) && !error;
    } catch (const std::exception&) {
        return false;
    }
}

std::string RepairFailureText(const UpdateReport& report) {
    std::string text;
    for (const ModelResult& result : report.Results) {
        if (result.Model.empty() && result.Detail.empty()) {
            continue;
        }
        if (!text.empty()) {
            text += '\n';
        }
        if (!result.Model.empty() && !result.Detail.empty()) {
            text += result.Model;
            text += ": ";
            text += result.Detail;
        } else if (!result.Detail.empty()) {
            text += result.Detail;
        } else {
            text += result.Model;
        }
    }
    if (text.empty()) {
        text = "The VPK was not modified.";
    }
    return text;
}

LibrarySettings LoadLibrarySettings(const std::filesystem::path& store_directory) {
    const SettingsInspection inspection = InspectLibrarySettings(store_directory);
    LibrarySettings settings;
    settings.GameDirectory = inspection.GameDirectory;
    return settings;
}

void SaveLibrarySettings(const std::filesystem::path& store_directory, const LibrarySettings& settings) {
    std::error_code error;
    std::filesystem::create_directories(store_directory, error);
    if (error) {
        throw IoError("Could not save the library settings.");
    }
    nlohmann::json body = nlohmann::json::object();
    body["gameDirectory"] = settings.GameDirectory;
    const std::string text = body.dump();
    const std::filesystem::path path = store_directory / "settings.json";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw IoError("Could not save the library settings.");
    }
    output << text;
    if (!output) {
        throw IoError("Could not save the library settings.");
    }
}

}  // namespace skeleton_repair
