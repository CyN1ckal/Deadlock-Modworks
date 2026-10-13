#pragma once

#include "cancellation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Deadlock mounts citadel\addons\pakNN_dir.vpk for NN from 01 to 99. A lower number wins when two mods
// contain the same file.
inline constexpr int kFirstAddonSlot = 1;
inline constexpr int kLastAddonSlot = 99;
// The record of the files Modworks wrote, inside the addons folder.
inline constexpr const wchar_t* kAddonsManifestName = L".modworks.json";

// "pak07_dir.vpk" for 7.
std::string AddonSlotName(int slot);
// The slot NN of "pakNN_dir.vpk" and its chunks "pakNN_000.vpk", without case. Nothing for any other name.
std::optional<int> AddonSlotOf(std::string_view file_name);

// One stored mod to place in addons.
struct DeployMod {
    std::string ContentId;
    std::string Name;
    std::filesystem::path Source;
};

// A file Modworks wrote. Size and Stamp tell whether it changed since.
struct AddonFile {
    int Slot = 0;
    std::string ContentId;
    std::string Name;
    uint64_t Size = 0;
    int64_t Stamp = 0;
    bool operator==(const AddonFile&) const = default;
};

struct AddonsScan {
    // Files Modworks wrote that are still exactly as it wrote them.
    std::vector<AddonFile> Managed;
    // Slots used by any other file named like a mounted mod. Modworks never writes, moves or removes these.
    std::set<int> Foreign;
    // Foreign file names, for display.
    std::vector<std::string> ForeignFiles;
    // Problems that did not stop the scan, such as a managed file changed by hand.
    std::vector<std::string> Notes;
};

// Reads the manifest and the folder. A missing folder scans as empty.
AddonsScan ScanAddons(const std::filesystem::path& addons);

// The slot each enabled mod should use: the lowest free slots, in priority order, skipping foreign slots.
// InvalidOperation when there are not enough free slots.
std::vector<int> AssignAddonSlots(const std::set<int>& foreign, size_t count);

struct DeployResult {
    size_t Copied = 0;
    size_t Moved = 0;
    size_t Removed = 0;
    size_t Kept = 0;
    std::vector<AddonFile> Deployed;
    std::vector<std::string> Notes;
};

// Bytes copied so far and the total to copy.
using DeployProgress = std::function<void(uint64_t, uint64_t)>;

// Makes the addons folder hold exactly `enabled`, highest priority first, as pakNN_dir.vpk files: mods already in
// place are kept, moved ones are renamed, new ones are copied, and Modworks files no longer wanted are removed.
// Foreign files are never touched. The manifest is saved even when this throws partway, so every file Modworks
// wrote stays recorded. The game must be closed; see IsDeadlockRunning.
DeployResult DeployAddons(const std::filesystem::path& addons, const std::vector<DeployMod>& enabled,
    CancellationToken cancellation = {}, const DeployProgress& progress = {});

// True while deadlock.exe is running.
bool IsDeadlockRunning();

// Two or more enabled mods with the same file. The first mod in priority order wins.
struct ModConflict {
    std::string Winner;
    std::string Loser;
    size_t Files = 0;
    // Up to five of the shared paths.
    std::vector<std::string> Examples;
};
// Every pair of mods that share files, by content id. Mods that cannot be opened are skipped.
std::vector<ModConflict> FindModConflicts(const std::vector<DeployMod>& enabled, CancellationToken cancellation = {});

// gameinfo.gi must list "Game citadel/addons" in SearchPaths for the game to load addons.
bool GameInfoMountsAddons(std::string_view gameinfo);
// The text with that line added before the first "Game citadel" line of SearchPaths.
// InvalidData when the file has no SearchPaths block.
std::string AddAddonsToGameInfo(std::string_view gameinfo);
// The citadel folder that holds gameinfo.gi and addons, from a game archive or folder path.
std::optional<std::filesystem::path> CitadelFolder(const std::filesystem::path& game);
// Adds the addons search path to citadel\gameinfo.gi after copying it to gameinfo.gi.modworks.bak.
void PatchGameInfo(const std::filesystem::path& citadel);

}  // namespace skeleton_repair
