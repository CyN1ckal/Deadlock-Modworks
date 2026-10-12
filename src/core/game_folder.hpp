#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// Library folders listed in Steam's libraryfolders.vdf, in file order.
std::vector<std::string> ParseSteamLibraryPaths(std::string_view vdf);

// The installed Deadlock game/citadel folder, found through the Steam registry key and library list.
std::optional<std::string> FindGameFolder();

}  // namespace skeleton_repair
