#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Library folders listed in Steam's libraryfolders.vdf, in file order.
std::vector<std::string> ParseSteamLibraryPaths(std::string_view vdf);

// The installed Deadlock game/citadel folder, found through the Steam registry key and library list.
std::optional<std::string> FindGameFolder();

// The ClientVersion line of a steam.inf, or nothing when the text has none.
std::optional<std::string> ParseClientVersion(std::string_view steam_inf);

// Names the installed build of the game in `citadel`, so a later launch can tell that Deadlock updated. It is
// steam.inf's ClientVersion when the folder has one, and otherwise the size and write time of pak01_dir.vpk.
// Empty when neither can be read.
std::string GameBuildId(const std::filesystem::path& citadel);

}  // namespace skeleton_repair
