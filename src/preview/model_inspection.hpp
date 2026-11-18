#pragma once

#include "archive_cache.hpp"
#include "comparison_view.hpp"
#include "model_geometry.hpp"
#include "model_viewport.hpp"
#include "resource_reader.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

using ArchivePtr = std::shared_ptr<const IndexedArchive>;

// How the game resolves a mod's files: the mod archive overrides the game's. The reader holds its own archive
// references, so worker threads can use it while the UI changes archives. Either archive may be null.
ResourceReader ModThenGame(ArchivePtr mod, ArchivePtr game);
// The reader the comparison view uses: the game version reads the game only, the others read ModThenGame.
ComparisonView::VersionReader VersionReaderFor(ArchivePtr mod, ArchivePtr game);

// The connected game archive and its animation clips. The game directory is huge, so both load off the UI thread.
struct LoadedGame {
    ArchivePtr Archive;
    std::vector<std::string> Clips;
};
LoadedGame LoadGame(ArchiveCache& archives, const std::filesystem::path& pak);

// Accepts the game/citadel folder or its pak01_dir.vpk, like the updater's game directory.
std::filesystem::path GamePak(const std::filesystem::path& chosen);

// Everything the inspector shows for one model, decoded on a worker thread.
struct Inspection {
    ArchivePtr Mod;
    ArchivePtr Game;
    std::string Model;
    std::string Title;
    std::optional<PreparedModel> Preview;
    std::string PreviewError;
    std::optional<std::vector<GeometryBone>> Animation;
    std::vector<std::string> Clips;
    std::optional<ComparisonView::Prepared> Comparison;
    std::string ComparisonError;
};

// One model of a mod archive, with its comparison against `game` when one is connected. An empty `model` picks the
// archive's first model. Problems, cancellation included, are reported in the result instead of thrown.
Inspection InspectArchiveModel(ArchiveCache& archives, const std::filesystem::path& archive, std::string model, ArchivePtr game,
    CancellationToken cancellation);
// A loose .vmdl_c file. It has no archive to compare against.
Inspection InspectModelFile(const std::filesystem::path& path);

}  // namespace skeleton_repair
