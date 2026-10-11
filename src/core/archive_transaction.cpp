#include "archive_transaction.hpp"

#include "error.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>

namespace skeleton_repair {
namespace {

std::wstring FullPath(const std::filesystem::path& path) {
    const std::wstring raw = path.wstring();
    const DWORD required = GetFullPathNameW(raw.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw IoError("Could not resolve a full path.");
    }
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(raw.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        throw IoError("Could not resolve a full path.");
    }
    buffer.resize(written);
    return buffer;
}

bool PathsEqual(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string Hex(const uint8_t* bytes, size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        text[i * 2] = kDigits[bytes[i] >> 4];
        text[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    return text;
}

std::string NewId() {
    uint8_t bytes[16];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw IoError("Could not create an archive id.");
    }
    return Hex(bytes, sizeof(bytes));
}

std::string UtcStamp() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream text;
    text << std::setfill('0')
         << std::setw(4) << time.wYear
         << std::setw(2) << time.wMonth
         << std::setw(2) << time.wDay << '-'
         << std::setw(2) << time.wHour
         << std::setw(2) << time.wMinute
         << std::setw(2) << time.wSecond;
    return text.str();
}

struct IgnoreCaseLess {
    bool operator()(const std::string& left, const std::string& right) const {
        return CompareIgnoreCase(left, right) < 0;
    }
};

class TempFile {
public:
    explicit TempFile(std::wstring path) : path_(std::move(path)) {}
    ~TempFile() { Delete(); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::wstring& Path() const { return path_; }

    void Delete() {
        if (!path_.empty()) {
            DeleteFileW(path_.c_str());
        }
    }

private:
    std::wstring path_;
};

}  // namespace

std::optional<std::filesystem::path> ArchiveTransaction::Write(
    const VpkArchive& source,
    const std::filesystem::path& source_path,
    const std::filesystem::path& destination,
    const std::vector<VpkChange>& changes,
    bool replace_source,
    CancellationToken cancellation,
    const std::function<void()>& before_commit) {
    const std::wstring source_full = FullPath(source_path);
    const std::wstring destination_full = FullPath(destination);
    if (replace_source != PathsEqual(source_full, destination_full)) {
        throw InvalidOperation("Replacement destination does not match the source mode.");
    }
    if (!replace_source && FileExists(destination_full)) {
        throw IoError("Output already exists. Choose a new filename; existing files will not be overwritten.");
    }
    std::filesystem::create_directories(std::filesystem::path(destination_full).parent_path());

    ReadLease lease = source.LockSources();
    const std::string id = NewId();
    TempFile temp(destination_full + L"." + Widen(id) + L".new");
    try {
        cancellation.ThrowIfCancelled();
        source.Write(temp.Path(), changes, cancellation);
        cancellation.ThrowIfCancelled();
        const VpkArchive check = VpkArchive::Open(temp.Path());
        check.Validate(cancellation);

        std::set<std::string, IgnoreCaseLess> expected;
        for (const VpkEntry& entry : source.Entries()) {
            expected.insert(entry.FullPath);
        }
        for (const VpkChange& change : changes) {
            if (!change.Data.has_value()) {
                auto it = expected.find(change.Path);
                if (it != expected.end()) {
                    expected.erase(it);
                }
            } else {
                expected.insert(change.Path);
            }
        }
        std::set<std::string, IgnoreCaseLess> actual;
        for (const VpkEntry& entry : check.Entries()) {
            actual.insert(entry.FullPath);
        }
        if (expected != actual) {
            throw InvalidData("The output archive does not contain the expected resources.");
        }
        for (const VpkChange& change : changes) {
            if (!change.Data.has_value()) {
                continue;
            }
            const auto found = std::find_if(check.Entries().begin(), check.Entries().end(), [&](const VpkEntry& entry) {
                return EqualsIgnoreCase(entry.FullPath, change.Path);
            });
            if (found == check.Entries().end() || check.Read(*found) != *change.Data) {
                throw InvalidData("Output verification failed for " + change.Path + ".");
            }
        }
        cancellation.ThrowIfCancelled();
        lease.Release();
        if (before_commit) {
            before_commit();
        }
        if (replace_source) {
            const std::wstring backup = source_full + L".bak-" + Widen(UtcStamp()) + L"-" + Widen(id.substr(0, 8));
            if (!ReplaceFileW(destination_full.c_str(), temp.Path().c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
                throw IoError("Could not replace the archive. " + std::to_string(GetLastError()));
            }
            return std::filesystem::path(backup);
        }
        if (!MoveFileExW(temp.Path().c_str(), destination_full.c_str(), MOVEFILE_WRITE_THROUGH)) {
            throw IoError("Could not publish the archive. " + std::to_string(GetLastError()));
        }
        return std::nullopt;
    } catch (...) {
        temp.Delete();
        throw;
    }
}

}  // namespace skeleton_repair
