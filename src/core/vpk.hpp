#pragma once

#include "cancellation.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

struct VpkEntry {
    std::string FullPath;
    uint32_t Crc32 = 0;
    uint16_t PreloadBytes = 0;
    uint16_t ArchiveIndex = 0;
    uint32_t Offset = 0;
    uint32_t Length = 0;
    std::vector<uint8_t> PreloadData;
};

// A missing Data value removes the path. A path that is not already in the archive is added.
struct VpkChange {
    std::string Path;
    std::optional<std::vector<uint8_t>> Data;
};

class ReadLease {
public:
    ReadLease();
    ~ReadLease();
    ReadLease(ReadLease&&) noexcept;
    ReadLease& operator=(ReadLease&&) noexcept;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;

    void Release();

private:
    friend class VpkArchive;
    struct State;
    std::unique_ptr<State> state_;
};

// Source 2 directory VPK. Reads chunked game archives and writes a single embedded mod archive.
class VpkArchive {
public:
    static constexpr uint16_t EmbeddedArchive = 0x7FFF;

    static VpkArchive Open(const std::filesystem::path& directory_path);

    const std::vector<VpkEntry>& Entries() const { return entries_; }

    ReadLease LockSources() const;
    std::vector<uint8_t> Read(const VpkEntry& entry, CancellationToken cancellation = {}) const;
    void CopyTo(const VpkEntry& entry, const std::function<void(const uint8_t*, size_t)>& write,
        CancellationToken cancellation = {}) const;
    void Validate(CancellationToken cancellation = {}) const;
    void Write(const std::filesystem::path& destination, const std::vector<VpkChange>& changes,
        CancellationToken cancellation = {}) const;

    static uint32_t Crc(const uint8_t* data, size_t size);
    static uint32_t Crc(const std::vector<uint8_t>& data) { return Crc(data.data(), data.size()); }
    static std::string Normalize(std::string path);

private:
    VpkArchive(std::filesystem::path directory, std::wstring base_name, std::vector<VpkEntry> entries,
        uint32_t file_data_size, int32_t tree_size);

    std::filesystem::path ChunkPath(uint16_t index) const;
    int64_t DataOffset(const VpkEntry& entry) const;

    std::filesystem::path directory_;
    std::wstring base_name_;
    uint32_t file_data_size_ = 0;
    int32_t tree_size_ = 0;
    std::vector<VpkEntry> entries_;
};

}  // namespace skeleton_repair
