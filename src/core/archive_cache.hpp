#pragma once

#include "cancellation.hpp"
#include "vpk.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace skeleton_repair {

// An opened VPK with a case-insensitive path index. Immutable, so any thread may read it.
class IndexedArchive {
public:
    explicit IndexedArchive(VpkArchive archive);
    IndexedArchive(const IndexedArchive&) = delete;
    IndexedArchive& operator=(const IndexedArchive&) = delete;

    const VpkArchive& Archive() const { return archive_; }
    const VpkEntry* Find(std::string_view path) const;
    std::optional<std::vector<uint8_t>> Read(std::string_view path, CancellationToken cancellation = {}) const;

private:
    VpkArchive archive_;
    std::unordered_map<std::string, const VpkEntry*> index_;
};

// Opens each archive once and shares it between threads. Concurrent requests for the same path wait for one open.
class ArchiveCache {
public:
    // Throws when the archive cannot be opened; a failed open is retried by the next request.
    std::shared_ptr<const IndexedArchive> Get(const std::filesystem::path& path);
    // Forgets every archive, for example after files were replaced. Holders keep their copies.
    void Clear();
    void Remove(const std::vector<std::filesystem::path>& paths);

private:
    static std::wstring Key(const std::filesystem::path& path);
    using Entry = std::shared_future<std::shared_ptr<const IndexedArchive>>;
    std::mutex mutex_;
    std::map<std::wstring, Entry> archives_;
};

}  // namespace skeleton_repair
