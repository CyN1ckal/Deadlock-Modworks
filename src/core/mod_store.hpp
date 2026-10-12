#pragma once
#include "model_library.hpp"
#include <functional>
#include <optional>
#include <string>

namespace skeleton_repair {
// Reports completed and total steps of a long store operation.
using StoreProgress = std::function<void(size_t, size_t)>;

// Keeps imported mods, the collection and rendered thumbnails in one SQLite file, library.db, that can be shared.
// Each mod file is stored once per SHA-256, so files repeated across mods or imports take no extra space.
// Opened mods are rebuilt from the database under cache\, which holds nothing that cannot be rebuilt.
class LocalModStore {
public:
    explicit LocalModStore(std::filesystem::path root);
    static std::filesystem::path DefaultRoot();
    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path Database() const { return root_ / L"library.db"; }
    // Stores the archive's files and returns its copy rebuilt from the database.
    // Identical content is stored once, whatever its path, name or timestamps.
    std::filesystem::path Import(const std::filesystem::path& source, CancellationToken cancellation = {}) const;
    // The saved collection, with each source rebuilt from the database. The first load also merges the folder-based
    // store used by earlier versions, which is left in place. `notice` receives problems that did not stop the load.
    ModelLibrary Load(CancellationToken cancellation = {}, const StoreProgress& progress = {}, std::string* notice = nullptr) const;
    void Save(const ModelLibrary& library) const;
    // Adds another library database's mods, collection and thumbnails. Nothing already stored is replaced or removed.
    void Merge(const std::filesystem::path& other, CancellationToken cancellation = {}) const;
    // Writes a compact standalone copy of the database, for sharing the whole library.
    void Export(const std::filesystem::path& destination) const;
    // The content identifier of an archive rebuilt by this store, or nothing for any other path.
    std::optional<std::string> ContentId(const std::filesystem::path& archive) const;
    // The archives an earlier version stored for this content, so their cached thumbnails can be reused.
    std::vector<std::filesystem::path> LegacyPaths(const std::filesystem::path& archive) const;
    std::optional<std::vector<uint8_t>> Thumbnail(const std::string& key) const;
    void SaveThumbnail(const std::string& key, const std::vector<uint8_t>& png) const;
private:
    std::filesystem::path root_;
};
}
