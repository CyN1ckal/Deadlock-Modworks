#pragma once
#include "vpk.hpp"
#include <filesystem>
#include <vector>

namespace skeleton_repair {
struct LibraryModel {
    std::filesystem::path Archive;
    std::string Model;
    bool operator==(const LibraryModel&) const = default;
};
struct ModelLibrary {
    std::vector<std::filesystem::path> Sources;
    std::vector<LibraryModel> Selected;
    std::filesystem::path GameArchive;
    bool operator==(const ModelLibrary&) const = default;
    void Save(const std::filesystem::path& path) const;
    static ModelLibrary Load(const std::filesystem::path& path);
};
// Only exports selected models and their reachable overrides from their own source.
// Game resources resolve dependencies but are not copied into the addon.
size_t ExportModelLibrary(const std::vector<LibraryModel>& models,
    const std::filesystem::path& game_archive, const std::filesystem::path& destination,
    CancellationToken cancellation = {});
}
