#include "store_jobs.hpp"

#include "content_store.hpp"
#include "updater.hpp"

#include <optional>

namespace skeleton_repair {

void AssembleIntoStore(const std::filesystem::path& store_directory, const SnapshotId& source, const BlobId& model,
    const std::filesystem::path& destination, CancellationToken cancellation) {
    ContentStore store = ContentStore::Open(store_directory);
    store.Assemble(source, {model}, destination, cancellation);
    store.ImportVpk(destination, cancellation);
}

UpdateReport RepairIntoStore(const std::filesystem::path& store_directory, const std::string& vpk_path,
    const std::string& game_directory, CancellationToken cancellation) {
    ContentStore store = ContentStore::Open(store_directory);
    return HeroModelUpdater::Update(game_directory, vpk_path, std::nullopt, true, true, {}, cancellation, false, &store);
}

}  // namespace skeleton_repair
