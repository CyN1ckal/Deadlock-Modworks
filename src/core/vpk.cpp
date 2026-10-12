#include "vpk.hpp"

#include "error.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace skeleton_repair {
namespace {

constexpr uint32_t kSignature = 0x55AA1234u;

uint32_t ReadU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

int32_t ReadI32(const uint8_t* data) {
    return static_cast<int32_t>(ReadU32(data));
}

void WriteU32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

void WriteU16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

uint32_t CheckedAdd(uint32_t left, uint32_t right) {
    const uint64_t sum = static_cast<uint64_t>(left) + right;
    if (sum > std::numeric_limits<uint32_t>::max()) {
        throw InvalidData("VPK data is too large.");
    }
    return static_cast<uint32_t>(sum);
}

std::string Win32Message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = "File error " + std::to_string(code);
    if (length != 0 && buffer != nullptr) {
        while (length > 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n')) {
            buffer[length - 1] = L'\0';
        }
        text += ": " + Narrow(buffer);
        LocalFree(buffer);
    }
    return text;
}

class FileHandle {
public:
    FileHandle() = default;
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() { reset(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

    void reset() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

FileHandle OpenFile(const std::filesystem::path& path, DWORD access, DWORD share, DWORD disposition) {
    const HANDLE handle = CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw IoError("Could not open " + path.string() + ". " + Win32Message(GetLastError()));
    }
    return FileHandle(handle);
}

int64_t FileSize(HANDLE handle) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        throw IoError("Could not read an archive length. " + Win32Message(GetLastError()));
    }
    return size.QuadPart;
}

void ReadExact(HANDLE handle, uint8_t* destination, DWORD size) {
    DWORD read_total = 0;
    while (read_total < size) {
        DWORD got = 0;
        if (!ReadFile(handle, destination + read_total, size - read_total, &got, nullptr)) {
            throw IoError("Could not read an archive. " + Win32Message(GetLastError()));
        }
        if (got == 0) {
            throw InvalidData("VPK directory or embedded data is outside the file.");
        }
        read_total += got;
    }
}

void WriteExact(HANDLE handle, const uint8_t* data, size_t size) {
    size_t written_total = 0;
    while (written_total < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written_total, std::numeric_limits<DWORD>::max()));
        DWORD wrote = 0;
        if (!WriteFile(handle, data + written_total, chunk, &wrote, nullptr) || wrote == 0) {
            throw IoError("Could not write an archive. " + Win32Message(GetLastError()));
        }
        written_total += wrote;
    }
}

void Seek(HANDLE handle, int64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        throw IoError("Could not seek in an archive. " + Win32Message(GetLastError()));
    }
}

const uint32_t* CrcTable() {
    static const auto table = [] {
        std::array<uint32_t, 256> values{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
            }
            values[i] = crc;
        }
        return values;
    }();
    return table.data();
}

class Crc32 {
public:
    void Append(const uint8_t* data, size_t size) {
        const uint32_t* table = CrcTable();
        for (size_t i = 0; i < size; ++i) {
            crc_ = table[(crc_ ^ data[i]) & 0xFFu] ^ (crc_ >> 8);
        }
    }

    uint32_t Value() const { return ~crc_; }

private:
    uint32_t crc_ = 0xFFFFFFFFu;
};

class Md5 {
public:
    Md5() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0
            || BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            throw IoError("MD5 is unavailable.");
        }
    }

    ~Md5() {
        if (hash_ != nullptr) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_ != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    void Append(const uint8_t* data, size_t size) {
        size_t done = 0;
        while (done < size) {
            const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - done, std::numeric_limits<ULONG>::max()));
            if (BCryptHashData(hash_, const_cast<PUCHAR>(data + done), chunk, 0) < 0) {
                throw IoError("MD5 update failed.");
            }
            done += chunk;
        }
    }

    std::array<uint8_t, 16> Finish() {
        std::array<uint8_t, 16> out{};
        if (BCryptFinishHash(hash_, out.data(), static_cast<ULONG>(out.size()), 0) < 0) {
            throw IoError("MD5 finish failed.");
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::array<uint8_t, 16> Md5Bytes(const uint8_t* data, size_t size) {
    Md5 hash;
    if (size != 0) {
        hash.Append(data, size);
    }
    return hash.Finish();
}

struct IgnoreCaseLess {
    bool operator()(const std::string& left, const std::string& right) const {
        return CompareIgnoreCase(left, right) < 0;
    }
};

std::string ReadString(const std::vector<uint8_t>& data, size_t& cursor) {
    if (cursor > data.size()) {
        throw InvalidData("Unterminated VPK string.");
    }
    const size_t start = cursor;
    while (cursor < data.size() && data[cursor] != 0) {
        ++cursor;
    }
    if (cursor >= data.size()) {
        throw InvalidData("Unterminated VPK string.");
    }
    std::string text(reinterpret_cast<const char*>(data.data() + start), cursor - start);
    ++cursor;
    return text;
}

void WriteString(std::vector<uint8_t>& out, const std::string& value) {
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
}

struct SplitPath {
    std::string Extension;
    std::string Directory;
    std::string Name;
};

SplitPath Split(std::string path) {
    path = VpkArchive::Normalize(std::move(path));
    const size_t slash = path.rfind('/');
    const std::string directory = slash == std::string::npos ? "" : path.substr(0, slash);
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = file.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        throw InvalidData("Cannot split VPK path " + path + ".");
    }
    return SplitPath{file.substr(dot + 1), directory, file.substr(0, dot)};
}

std::vector<VpkEntry> ParseTree(const std::vector<uint8_t>& tree) {
    std::vector<VpkEntry> entries;
    size_t cursor = 0;
    while (cursor < tree.size()) {
        std::string extension = ReadString(tree, cursor);
        if (extension.empty()) {
            break;
        }
        while (true) {
            std::string directory = ReadString(tree, cursor);
            if (directory.empty()) {
                break;
            }
            while (true) {
                std::string name = ReadString(tree, cursor);
                if (name.empty()) {
                    break;
                }
                if (cursor > tree.size() || tree.size() - cursor < 18) {
                    throw InvalidData("Truncated VPK directory entry.");
                }
                const uint32_t crc = ReadU32(tree.data() + cursor);
                const uint16_t preload = ReadU16(tree.data() + cursor + 4);
                const uint16_t archive = ReadU16(tree.data() + cursor + 6);
                const uint32_t offset = ReadU32(tree.data() + cursor + 8);
                const uint32_t length = ReadU32(tree.data() + cursor + 12);
                const uint16_t terminator = ReadU16(tree.data() + cursor + 16);
                if (terminator != 0xFFFF) {
                    throw InvalidData("Broken VPK directory entry.");
                }
                if (static_cast<size_t>(preload) > tree.size() - cursor - 18) {
                    throw InvalidData("Truncated VPK preload data.");
                }
                std::vector<uint8_t> preload_data(tree.begin() + static_cast<std::ptrdiff_t>(cursor + 18),
                    tree.begin() + static_cast<std::ptrdiff_t>(cursor + 18 + preload));
                cursor += 18 + preload;
                // A single space is the Source 2 marker for the archive root.
                if (directory == " ") {
                    directory.clear();
                }
                const std::string full = directory.empty() ? name + "." + extension : directory + "/" + name + "." + extension;
                entries.push_back(VpkEntry{full, crc, preload, archive, offset, length, std::move(preload_data)});
            }
        }
    }
    return entries;
}

struct PlannedFile {
    std::string Path;
    std::string Extension;
    std::string Directory;
    std::string Name;
    const VpkEntry* Source = nullptr;
    const std::vector<uint8_t>* Replacement = nullptr;
    const VpkNewFile* Generated = nullptr;
    uint32_t Length = 0;
    uint32_t Offset = 0;
};

const VpkChange* FindChange(const std::vector<VpkChange>& changes, const std::string& path) {
    const VpkChange* found = nullptr;
    for (const VpkChange& change : changes) {
        if (!EqualsIgnoreCase(change.Path, path)) {
            continue;
        }
        if (found != nullptr) {
            throw InvalidData("VPK changes contain duplicate paths.");
        }
        found = &change;
    }
    return found;
}

std::wstring StripDirSuffix(std::wstring name) {
    if (name.size() >= 4
        && CompareStringOrdinal(name.data() + name.size() - 4, 4, L"_dir", 4, TRUE) == CSTR_EQUAL) {
        name.resize(name.size() - 4);
    }
    return name;
}

using Sink = std::function<void(const uint8_t*, size_t)>;

// Lays out sorted files as one embedded archive. `copy` streams files that come from an existing archive.
void WritePlanned(const std::filesystem::path& destination, std::vector<PlannedFile>& planned,
    const std::function<void(const VpkEntry&, const Sink&)>& copy, CancellationToken cancellation) {
    std::stable_sort(planned.begin(), planned.end(), [](const PlannedFile& left, const PlannedFile& right) {
        if (const int extension = CompareOrdinal(left.Extension, right.Extension)) {
            return extension < 0;
        }
        if (const int directory = CompareOrdinal(left.Directory, right.Directory)) {
            return directory < 0;
        }
        return CompareOrdinal(left.Name, right.Name) < 0;
    });

    uint32_t offset = 0;
    for (PlannedFile& file : planned) {
        file.Offset = offset;
        offset = CheckedAdd(offset, file.Length);
    }

    std::vector<uint8_t> tree;
    for (size_t index = 0; index < planned.size();) {
        const std::string& extension = planned[index].Extension;
        WriteString(tree, extension);
        while (index < planned.size() && planned[index].Extension == extension) {
            const std::string& directory = planned[index].Directory;
            WriteString(tree, directory.empty() ? " " : directory);
            while (index < planned.size() && planned[index].Extension == extension && planned[index].Directory == directory) {
                const PlannedFile& file = planned[index];
                WriteString(tree, file.Name);
                uint8_t meta[18] = {};
                const uint32_t crc = file.Replacement != nullptr ? VpkArchive::Crc(*file.Replacement)
                    : file.Generated != nullptr ? file.Generated->Crc32 : file.Source->Crc32;
                WriteU32(meta, crc);
                WriteU16(meta + 6, VpkArchive::EmbeddedArchive);
                WriteU32(meta + 8, file.Offset);
                WriteU32(meta + 12, file.Length);
                WriteU16(meta + 16, 0xFFFF);
                tree.insert(tree.end(), meta, meta + 18);
                ++index;
            }
            tree.push_back(0);
        }
        tree.push_back(0);
    }
    tree.push_back(0);

    uint8_t header[28] = {};
    WriteU32(header, kSignature);
    WriteU32(header + 4, 2u);
    WriteU32(header + 8, static_cast<uint32_t>(tree.size()));
    WriteU32(header + 12, offset);
    WriteU32(header + 20, 48u);

    FileHandle output = OpenFile(destination, GENERIC_WRITE, 0, CREATE_ALWAYS);
    Md5 whole;
    auto emit = [&](const uint8_t* data, size_t size) {
        if (size == 0) {
            return;
        }
        WriteExact(output.get(), data, size);
        whole.Append(data, size);
    };
    emit(header, 28);
    emit(tree.data(), tree.size());
    for (const PlannedFile& file : planned) {
        cancellation.ThrowIfCancelled();
        if (file.Replacement != nullptr) {
            emit(file.Replacement->data(), file.Replacement->size());
        } else if (file.Generated != nullptr) {
            uint64_t written = 0;
            file.Generated->Write([&](const uint8_t* data, size_t size) { written += size; emit(data, size); });
            if (written != file.Length) {
                throw InvalidData(file.Path + " did not produce its recorded length.");
            }
        } else {
            copy(*file.Source, [&](const uint8_t* data, size_t size) { emit(data, size); });
        }
    }

    const std::array<uint8_t, 16> tree_md5 = Md5Bytes(tree.data(), tree.size());
    const std::array<uint8_t, 16> empty_md5 = Md5Bytes(nullptr, 0);
    emit(tree_md5.data(), tree_md5.size());
    emit(empty_md5.data(), empty_md5.size());
    const std::array<uint8_t, 16> file_md5 = whole.Finish();
    WriteExact(output.get(), file_md5.data(), file_md5.size());
}

}  // namespace

struct ReadLease::State {
    std::vector<FileHandle> handles;
};

ReadLease::ReadLease() = default;
ReadLease::~ReadLease() { Release(); }
ReadLease::ReadLease(ReadLease&&) noexcept = default;
ReadLease& ReadLease::operator=(ReadLease&&) noexcept = default;

void ReadLease::Release() {
    if (state_) {
        state_->handles.clear();
    }
}

VpkArchive::VpkArchive(std::filesystem::path directory, std::wstring base_name, std::vector<VpkEntry> entries,
    uint32_t file_data_size, int32_t tree_size)
    : directory_(std::move(directory)),
      base_name_(std::move(base_name)),
      file_data_size_(file_data_size),
      tree_size_(tree_size),
      entries_(std::move(entries)) {}

uint32_t VpkArchive::Crc(const uint8_t* data, size_t size) {
    Crc32 crc;
    if (data != nullptr && size != 0) {
        crc.Append(data, size);
    }
    return crc.Value();
}

std::string VpkArchive::Normalize(std::string path) {
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
    }
    size_t start = 0;
    while (start < path.size() && path[start] == '/') {
        ++start;
    }
    return path.substr(start);
}

VpkArchive VpkArchive::Open(const std::filesystem::path& directory_path) {
    FileHandle file = OpenFile(directory_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    const int64_t length = FileSize(file.get());
    if (length < 28) {
        throw InvalidData("VPK directory or embedded data is outside the file.");
    }
    uint8_t header[28];
    ReadExact(file.get(), header, 28);
    const uint32_t signature = ReadU32(header);
    const uint32_t version = ReadU32(header + 4);
    const int32_t tree_size = ReadI32(header + 8);
    const uint32_t file_data_size = ReadU32(header + 12);
    if (signature != kSignature || version != 2) {
        throw InvalidData(directory_path.filename().string() + " is not a Source 2 VPK.");
    }
    // Bytes 16..28 are the archive MD5, other MD5, and signature sizes.
    // Chunked game archives fill those in. The tree still begins after this header,
    // and Write() emits a new embedded mod archive instead of copying those sections.
    const int64_t tree_bytes = tree_size;
    const int64_t data_bytes = file_data_size;
    if (tree_size < 0 || tree_bytes > length - 28 || 28 + tree_bytes + data_bytes > length) {
        throw InvalidData("VPK directory or embedded data is outside the file.");
    }
    std::vector<uint8_t> tree(static_cast<size_t>(tree_size));
    if (tree_size > 0) {
        ReadExact(file.get(), tree.data(), static_cast<DWORD>(tree_size));
    }
    std::vector<VpkEntry> entries = ParseTree(tree);
    std::set<std::string, IgnoreCaseLess> seen;
    for (const VpkEntry& entry : entries) {
        if (!seen.insert(entry.FullPath).second) {
            throw InvalidData("VPK contains duplicate resource paths.");
        }
    }
    return VpkArchive(directory_path, StripDirSuffix(directory_path.stem().wstring()), std::move(entries), file_data_size, tree_size);
}

std::filesystem::path VpkArchive::ChunkPath(uint16_t index) const {
    wchar_t digits[8];
    swprintf_s(digits, L"%03u", static_cast<unsigned>(index));
    return directory_.parent_path() / (base_name_ + L"_" + digits + L".vpk");
}

int64_t VpkArchive::DataOffset(const VpkEntry& entry) const {
    if (entry.ArchiveIndex == EmbeddedArchive) {
        return 28 + static_cast<int64_t>(tree_size_) + entry.Offset;
    }
    return entry.Offset;
}

ReadLease VpkArchive::LockSources() const {
    ReadLease lease;
    lease.state_ = std::make_unique<ReadLease::State>();
    try {
        lease.state_->handles.push_back(OpenFile(directory_, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING));
        std::vector<uint16_t> indexes;
        for (const VpkEntry& entry : entries_) {
            if (entry.ArchiveIndex == EmbeddedArchive) {
                continue;
            }
            if (std::find(indexes.begin(), indexes.end(), entry.ArchiveIndex) == indexes.end()) {
                indexes.push_back(entry.ArchiveIndex);
            }
        }
        for (uint16_t index : indexes) {
            const std::filesystem::path chunk = ChunkPath(index);
            if (!std::filesystem::exists(chunk)) {
                throw IoError("Missing VPK chunk " + chunk.filename().string() + ".");
            }
            lease.state_->handles.push_back(OpenFile(chunk, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING));
        }
    } catch (...) {
        lease.Release();
        throw;
    }
    return lease;
}

void VpkArchive::CopyTo(const VpkEntry& entry, const std::function<void(const uint8_t*, size_t)>& write,
    CancellationToken cancellation) const {
    std::filesystem::path data_path = directory_;
    if (entry.ArchiveIndex != EmbeddedArchive) {
        data_path = ChunkPath(entry.ArchiveIndex);
        if (!std::filesystem::exists(data_path)) {
            throw IoError("Missing VPK chunk " + data_path.filename().string() + ".");
        }
    }
    FileHandle file = OpenFile(data_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING);
    const int64_t length = FileSize(file.get());
    const int64_t offset = DataOffset(entry);
    if (offset > length || entry.Length > static_cast<uint64_t>(length - offset)
        || (entry.ArchiveIndex == EmbeddedArchive
            && static_cast<int64_t>(entry.Offset) + entry.Length > file_data_size_)) {
        throw InvalidData(entry.FullPath + " data is outside its archive.");
    }
    Seek(file.get(), offset);

    Crc32 crc;
    if (entry.PreloadBytes > 0) {
        if (entry.PreloadData.size() != entry.PreloadBytes) {
            throw InvalidData(entry.FullPath + " has invalid preload data.");
        }
        write(entry.PreloadData.data(), entry.PreloadData.size());
        crc.Append(entry.PreloadData.data(), entry.PreloadData.size());
    }

    uint32_t remaining = entry.Length;
    std::vector<uint8_t> buffer(1024 * 1024);
    while (remaining > 0) {
        cancellation.ThrowIfCancelled();
        const DWORD ask = static_cast<DWORD>(std::min<uint32_t>(remaining, static_cast<uint32_t>(buffer.size())));
        DWORD got = 0;
        if (!ReadFile(file.get(), buffer.data(), ask, &got, nullptr)) {
            throw IoError("Could not read " + entry.FullPath + ". " + Win32Message(GetLastError()));
        }
        if (got == 0) {
            throw InvalidData(entry.FullPath);
        }
        write(buffer.data(), got);
        crc.Append(buffer.data(), got);
        remaining -= got;
    }
    if (crc.Value() != entry.Crc32) {
        throw InvalidData("CRC mismatch in " + entry.FullPath + "; the archive is damaged.");
    }
}

void VpkArchive::Create(const std::filesystem::path& destination, const std::vector<VpkNewFile>& files,
    CancellationToken cancellation) {
    std::set<std::string, IgnoreCaseLess> seen;
    std::vector<PlannedFile> planned;
    for (const VpkNewFile& file : files) {
        const std::string path = Normalize(file.Path);
        if (!seen.insert(path).second) {
            throw InvalidData("VPK files contain duplicate paths.");
        }
        if (file.Length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
            throw InvalidData("VPK entry is too large.");
        }
        const SplitPath parts = Split(path);
        planned.push_back(PlannedFile{path, parts.Extension, parts.Directory, parts.Name, nullptr, nullptr, &file, file.Length, 0});
    }
    WritePlanned(destination, planned, [](const VpkEntry&, const Sink&) {}, cancellation);
}

std::vector<uint8_t> VpkArchive::Read(const VpkEntry& entry, CancellationToken cancellation) const {
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(entry.Length) + entry.PreloadBytes);
    CopyTo(entry, [&](const uint8_t* data, size_t size) {
        output.insert(output.end(), data, data + size);
    }, cancellation);
    return output;
}

void VpkArchive::Validate(CancellationToken cancellation) const {
    for (const VpkEntry& entry : entries_) {
        cancellation.ThrowIfCancelled();
        CopyTo(entry, [](const uint8_t*, size_t) {}, cancellation);
    }
}

void VpkArchive::Write(const std::filesystem::path& destination, const std::vector<VpkChange>& changes,
    CancellationToken cancellation) const {
    for (size_t i = 0; i < changes.size(); ++i) {
        for (size_t j = i + 1; j < changes.size(); ++j) {
            if (EqualsIgnoreCase(changes[i].Path, changes[j].Path)) {
                throw InvalidData("VPK changes contain duplicate paths.");
            }
        }
    }

    std::set<std::string, IgnoreCaseLess> existing;
    for (const VpkEntry& entry : entries_) {
        existing.insert(entry.FullPath);
    }

    std::vector<PlannedFile> planned;
    for (const VpkEntry& entry : entries_) {
        if (const VpkChange* change = FindChange(changes, entry.FullPath)) {
            if (!change->Data.has_value()) {
                continue;
            }
            const SplitPath parts = Split(entry.FullPath);
            if (change->Data->size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
                throw InvalidData("VPK entry is too large.");
            }
            planned.push_back(PlannedFile{entry.FullPath, parts.Extension, parts.Directory, parts.Name,
                nullptr, &(*change->Data), nullptr, static_cast<uint32_t>(change->Data->size()), 0});
        } else {
            const SplitPath parts = Split(entry.FullPath);
            const uint32_t length = CheckedAdd(entry.Length, entry.PreloadBytes);
            if (length > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                throw InvalidData("VPK entry is too large.");
            }
            planned.push_back(PlannedFile{entry.FullPath, parts.Extension, parts.Directory, parts.Name,
                &entry, nullptr, nullptr, length, 0});
        }
    }
    for (const VpkChange& change : changes) {
        if (!change.Data.has_value() || existing.contains(change.Path)) {
            continue;
        }
        const std::string path = Normalize(change.Path);
        const SplitPath parts = Split(path);
        if (change.Data->size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            throw InvalidData("VPK entry is too large.");
        }
        planned.push_back(PlannedFile{path, parts.Extension, parts.Directory, parts.Name,
            nullptr, &(*change.Data), nullptr, static_cast<uint32_t>(change.Data->size()), 0});
    }

    WritePlanned(destination, planned, [&](const VpkEntry& entry, const Sink& sink) { CopyTo(entry, sink, cancellation); }, cancellation);
}

}  // namespace skeleton_repair
