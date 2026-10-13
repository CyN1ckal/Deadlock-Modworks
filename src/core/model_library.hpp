#pragma once
#include "library_types.hpp"
#include "vpk.hpp"
#include <filesystem>
#include <vector>

namespace skeleton_repair {
struct ModelLibrary {
    std::vector<std::filesystem::path> Sources;
    std::vector<LibraryModel> Selected;
    std::filesystem::path GameArchive;
    // Sources whose non-model files are in the pack. The list is the source, not each file:
    // a replaced import contributes whatever those files are now.
    std::vector<std::filesystem::path> Packages;
    bool operator==(const ModelLibrary&) const = default;
    void Save(const std::filesystem::path& path) const;
    static ModelLibrary Load(const std::filesystem::path& path);
};
// "3 hideout textures", "1 texture, 2 UI files". Paths under models/hideout/ are hideout textures.
std::string PackageSubtitle(const std::vector<std::string>& paths);
// Selected models contribute their dependency closure. Each package source also copies every
// non-model file as stored, without walking its references. An empty model list is allowed
// when a package expands to at least one file. `packages` defaults to none.
size_t ExportModelLibrary(const std::vector<LibraryModel>& models,
    const std::filesystem::path& game_archive, const std::filesystem::path& destination,
    CancellationToken cancellation = {},
    const std::vector<std::filesystem::path>& packages = {});
}
