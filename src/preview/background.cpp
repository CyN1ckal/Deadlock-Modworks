#include "background.hpp"

#include "strings.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>

namespace skeleton_repair {
namespace {

// ASCII case folding for the index. Paths with other characters fall back to a full comparison.
std::string Fold(std::string_view path) {
    std::string folded(path);
    for (char& character : folded) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return folded;
}

bool Ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char character) { return static_cast<unsigned char>(character) < 0x80; });
}

}  // namespace

IndexedArchive::IndexedArchive(VpkArchive archive) : archive_(std::move(archive)) {
    index_.reserve(archive_.Entries().size());
    for (const VpkEntry& entry : archive_.Entries()) index_.emplace(Fold(entry.FullPath), &entry);
}

const VpkEntry* IndexedArchive::Find(std::string_view path) const {
    if (const auto found = index_.find(Fold(path)); found != index_.end()) return found->second;
    if (Ascii(path)) return nullptr;
    for (const VpkEntry& entry : archive_.Entries()) {
        if (EqualsIgnoreCase(entry.FullPath, path)) return &entry;
    }
    return nullptr;
}

std::optional<std::vector<uint8_t>> IndexedArchive::Read(std::string_view path, CancellationToken cancellation) const {
    const VpkEntry* entry = Find(path);
    if (entry == nullptr) return std::nullopt;
    return archive_.Read(*entry, cancellation);
}

std::wstring ArchiveCache::Key(const std::filesystem::path& path) {
    std::wstring key = std::filesystem::absolute(path).lexically_normal().wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t character) { return static_cast<wchar_t>(towlower(character)); });
    return key;
}

std::shared_ptr<const IndexedArchive> ArchiveCache::Get(const std::filesystem::path& path) {
    const std::wstring key = Key(path);
    std::promise<std::shared_ptr<const IndexedArchive>> promise;
    Entry entry;
    bool opener = false;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = archives_.find(key); found != archives_.end()) {
            entry = found->second;
        } else {
            entry = promise.get_future().share();
            archives_.emplace(key, entry);
            opener = true;
        }
    }
    if (opener) {
        try {
            promise.set_value(std::make_shared<const IndexedArchive>(VpkArchive::Open(path)));
        } catch (...) {
            promise.set_exception(std::current_exception());
            std::lock_guard lock(mutex_);
            if (const auto found = archives_.find(key); found != archives_.end()
                && found->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                try { found->second.get(); } catch (...) { archives_.erase(found); }
            }
        }
    }
    return entry.get();
}

void ArchiveCache::Clear() {
    std::lock_guard lock(mutex_);
    archives_.clear();
}

void ArchiveCache::Remove(const std::vector<std::filesystem::path>& paths) {
    std::lock_guard lock(mutex_);
    for (const auto& path : paths) archives_.erase(Key(path));
}

}  // namespace skeleton_repair
