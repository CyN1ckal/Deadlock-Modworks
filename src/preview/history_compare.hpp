#pragma once

#include "content_types.hpp"
#include "report.hpp"

#include <filesystem>
#include <string>

namespace skeleton_repair {

inline constexpr const char* kNoRecordedTransform = "No recorded transform between these versions.";
inline constexpr const char* kRepairSourceMissing = "The imported VPK is no longer at its original path.";
inline constexpr const char* kRepairNeedsGameFolder = "Set the game folder before repairing.";
inline constexpr const char* kCompareNeedsGameFolder = "Set the game folder to compare this model with the installed game.";

// Caption for two history versions. A direct edge contributes both 12-hex prefixes and its relation.
// Versions with no direct edge use kNoRecordedTransform and nothing else.
std::string HistoryCompareCaption(const History& history, const BlobId& first, const BlobId& second);

// The first direct edge between the two blobs, in either direction. Null when the blobs are the same
// or no edge joins them. The pointer addresses history.Links.
const HistoryLink* DirectHistoryLink(const History& history, const BlobId& first, const BlobId& second);

struct RepairGate {
    bool Enabled = false;
    // Exact disabled message. Null when Enabled is true.
    const char* Message = nullptr;
};

// Path missing wins over a missing game folder. Both true enables repair.
RepairGate RepairGateFor(bool observation_file_exists, bool game_folder_set);

// UTF-8 absolute path of a regular file. Empty, missing, and unreadable paths are false. Does not throw.
bool ObservationFileExists(const std::string& absolute_path);

// Detail lines from a repair that did not write. The UI shows this and keeps the input snapshot.
std::string RepairFailureText(const UpdateReport& report);

struct LibrarySettings {
    std::string GameDirectory;
};

// Missing or corrupt settings.json yields an empty directory and does not throw.
LibrarySettings LoadLibrarySettings(const std::filesystem::path& store_directory);
void SaveLibrarySettings(const std::filesystem::path& store_directory, const LibrarySettings& settings);

}  // namespace skeleton_repair
