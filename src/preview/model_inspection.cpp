#include "model_inspection.hpp"

#include "profile.hpp"
#include "model_versions.hpp"
#include "strings.hpp"
#include "version_builder.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace skeleton_repair {
namespace {

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Could not open " + Narrow(path.wstring()) + ".");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsClip(std::string_view path) { return EndsWith(path, ".vnmclip_c"); }

// Clips in the model's folder (".../clips/", ".../clip/"), from the mod and the game.
std::vector<std::string> ClipsFor(const std::string& model_path, const IndexedArchive* mod, const IndexedArchive* game) {
    const size_t slash = model_path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : model_path.substr(0, slash + 1);
    std::vector<std::string> clips;
    for (const IndexedArchive* archive : {mod, game}) {
        if (archive == nullptr) continue;
        for (const VpkEntry& entry : archive->Archive().Entries()) {
            if (entry.FullPath.size() > folder.size() && entry.FullPath.compare(0, folder.size(), folder) == 0 && IsClip(entry.FullPath)
                && std::find(clips.begin(), clips.end(), entry.FullPath) == clips.end()) {
                clips.push_back(entry.FullPath);
            }
        }
    }
    return clips;
}

std::optional<std::string> SkeletonPathOf(const std::vector<uint8_t>& model) {
    try {
        return AnimationSkeletonPath(DecodeModelGeometry(model));
    } catch (const std::exception&) {
        // The comparison view reports the decode error itself.
        return std::nullopt;
    }
}

ModelSources Sources(const std::string& path, std::vector<uint8_t> bytes, const IndexedArchive& mod, const IndexedArchive& game) {
    ModelSources sources;
    sources.ModelPath = path;
    std::optional<std::string> skeleton = SkeletonPathOf(bytes);
    sources.ModModel = std::move(bytes);
    if (const VpkEntry* game_model = game.Find(path)) {
        sources.GameModelPath = game_model->FullPath;
        sources.GameModel = game.Archive().Read(*game_model);
        if (!skeleton) skeleton = SkeletonPathOf(*sources.GameModel);
    }
    if (skeleton) {
        sources.SkeletonPath = *skeleton;
        sources.ModSkeleton = mod.Read(*skeleton);
        sources.GameSkeleton = game.Read(*skeleton);
    }
    return sources;
}

}  // namespace

ResourceReader ModThenGame(ArchivePtr mod, ArchivePtr game) {
    return [mod = std::move(mod), game = std::move(game)](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (mod) {
            if (auto bytes = mod->Read(path)) return bytes;
        }
        return game ? game->Read(path) : std::nullopt;
    };
}

ComparisonView::VersionReader VersionReaderFor(ArchivePtr mod, ArchivePtr game) {
    return [read = ModThenGame(mod, game), game](ModelVersion version, const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (version == ModelVersion::Game) return game ? game->Read(path) : std::nullopt;
        return read(path);
    };
}

LoadedGame LoadGame(ArchiveCache& archives, const std::filesystem::path& pak) {
    ZoneScopedN("Open game archive");
    LoadedGame loaded;
    loaded.Archive = archives.Get(pak);
    {
        ZoneScopedN("Collect game clips");
        for (const VpkEntry& entry : loaded.Archive->Archive().Entries()) {
            if (IsClip(entry.FullPath)) loaded.Clips.push_back(entry.FullPath);
        }
    }
    return loaded;
}

std::filesystem::path GamePak(const std::filesystem::path& chosen) {
    if (EndsWith(Narrow(chosen.wstring()), ".vpk")) return chosen;
    return chosen / L"pak01_dir.vpk";
}

Inspection InspectArchiveModel(ArchiveCache& archives, const std::filesystem::path& archive, std::string model, ArchivePtr game,
    CancellationToken cancellation) {
    Inspection result;
    result.Game = game;
    result.Title = Narrow(archive.filename().wstring());
    try {
        result.Mod = archives.Get(archive);
    } catch (const std::exception& error) {
        result.Title = Narrow(archive.wstring());
        result.PreviewError = result.ComparisonError = error.what();
        return result;
    }
    if (model.empty()) {
        for (const VpkEntry& entry : result.Mod->Archive().Entries()) {
            if (EndsWith(entry.FullPath, ".vmdl_c") && (model.empty() || entry.FullPath < model)) model = entry.FullPath;
        }
        if (model.empty()) {
            result.PreviewError = "This archive contains no compiled models.";
            return result;
        }
    }
    result.Model = model;
    result.Title = model;
    const VpkEntry* entry = result.Mod->Find(model);
    if (entry == nullptr) {
        result.PreviewError = result.ComparisonError = "This model is no longer in " + Narrow(archive.filename().wstring()) + ".";
        return result;
    }
    const ResourceReader read = ModThenGame(result.Mod, game);
    std::vector<uint8_t> bytes;
    try {
        bytes = result.Mod->Archive().Read(*entry, cancellation);
        const ModelGeometry geometry = DecodeModelGeometry(bytes);
        if (const std::optional<std::string> skeleton = AnimationSkeletonPath(geometry)) {
            if (const auto skeleton_bytes = read(*skeleton)) {
                try {
                    result.Animation = DecodeAnimationSkeleton(*skeleton_bytes);
                } catch (const std::exception&) {
                    // Shown as "no animation skeleton"; the comparison view reports the decode error.
                }
            }
        }
        result.Clips = ClipsFor(model, result.Mod.get(), game.get());
        cancellation.ThrowIfCancelled();
        result.Preview = PrepareModel(geometry, &read);
    } catch (const std::exception& error) {
        result.PreviewError = error.what();
    }
    if (!game) {
        result.ComparisonError = "Set the game folder to compare this model with the installed game.";
        return result;
    }
    try {
        cancellation.ThrowIfCancelled();
        result.Comparison = ComparisonView::Prepare(BuildModelVersions(Sources(model, std::move(bytes), *result.Mod, *game)),
            VersionReaderFor(result.Mod, game), result.Preview ? &*result.Preview : nullptr);
    } catch (const std::exception& error) {
        result.ComparisonError = error.what();
    }
    return result;
}

Inspection InspectModelFile(const std::filesystem::path& path) {
    Inspection result;
    result.Title = Narrow(path.wstring());
    try {
        result.Preview = PrepareModel(DecodeModelGeometry(ReadFile(path)));
    } catch (const std::exception& error) {
        result.PreviewError = error.what();
    }
    result.ComparisonError = "Comparison needs a model opened from a mod VPK.";
    return result;
}

}  // namespace skeleton_repair
