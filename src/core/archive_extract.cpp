#include "archive_extract.hpp"

#include "error.hpp"
#include "strings.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

bool HasExtension(const fs::path& path, const wchar_t* extension) { return Lower(path.extension().wstring()) == extension; }

// name_000.vpk: a data chunk of name_dir.vpk.
bool IsChunkName(const std::wstring& name) {
    const auto lower = Lower(name);
    if (lower.size() < 9 || !lower.ends_with(L".vpk")) return false;
    const auto stem = lower.substr(0, lower.size() - 4);
    return stem[stem.size() - 4] == L'_'
        && std::all_of(stem.end() - 3, stem.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

// The file name of an entry, with characters Windows refuses replaced. Empty when nothing usable is left.
std::wstring SafeFileName(std::wstring name) {
    for (auto& c : name) if (c < 32 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos) c = L'_';
    const auto trim = name.find_last_not_of(L" .");
    name.erase(trim == std::wstring::npos ? 0 : trim + 1);
    if (name == L"." || name == L"..") return {};
    return name;
}

struct ReadArchive {
    archive* Handle = archive_read_new();
    ~ReadArchive() { if (Handle) archive_read_free(Handle); }
    [[noreturn]] void Fail(const std::string& what) const {
        const char* message = Handle ? archive_error_string(Handle) : nullptr;
        throw InvalidData(what + (message ? std::string(": ") + message : std::string(".")));
    }
};

// Each chunked mod named pak01 is renamed, because the library treats pak01_dir.vpk beside pak01_000.vpk as the game.
void RenameGameLikeChunks(const fs::path& folder) {
    std::error_code error;
    if (!fs::exists(folder / L"pak01_000.vpk", error)) return;
    for (const auto& entry : fs::directory_iterator(folder)) {
        const auto name = Lower(entry.path().filename().wstring());
        if (!name.starts_with(L"pak01_")) continue;
        fs::rename(entry.path(), folder / (L"mod01_" + entry.path().filename().wstring().substr(6)));
    }
}
}  // namespace

bool IsModArchive(const fs::path& path) {
    return HasExtension(path, L".zip") || HasExtension(path, L".7z") || HasExtension(path, L".rar");
}

ExtractedMods ExtractModArchive(const fs::path& archive_path, const fs::path& destination, CancellationToken cancellation,
    const ExtractProgress& progress, const ExtractLimits& limits) {
    std::error_code error;
    if (fs::exists(destination, error) && !fs::is_empty(destination, error))
        throw InvalidOperation("The extraction folder must be empty.");
    fs::create_directories(destination);

    ReadArchive reader;
    if (!reader.Handle) throw IoError("Could not start reading the archive.");
    archive_read_support_format_zip(reader.Handle);
    archive_read_support_format_7zip(reader.Handle);
    archive_read_support_format_rar(reader.Handle);
    archive_read_support_format_rar5(reader.Handle);
    archive_read_support_format_tar(reader.Handle);
    archive_read_support_filter_all(reader.Handle);
    if (archive_read_open_filename_w(reader.Handle, archive_path.c_str(), 1 << 16) != ARCHIVE_OK)
        reader.Fail("Could not open " + Narrow(archive_path.filename().wstring()));

    ExtractedMods result;
    std::map<std::wstring, size_t> folders;
    uint64_t total = 0;
    size_t entries = 0;
    std::vector<char> buffer(1 << 16);
    archive_entry* entry = nullptr;
    for (;;) {
        cancellation.ThrowIfCancelled();
        const int status = archive_read_next_header(reader.Handle, &entry);
        if (status == ARCHIVE_EOF) break;
        if (status < ARCHIVE_WARN) reader.Fail("The archive is damaged");
        if (++entries > limits.MaxEntries) throw InvalidData("The archive has too many entries.");
        if (archive_entry_filetype(entry) != AE_IFREG) continue;

        std::wstring entry_path;
        if (const wchar_t* wide = archive_entry_pathname_w(entry)) entry_path = wide;
        else if (const char* narrow = archive_entry_pathname_utf8(entry)) entry_path = Widen(narrow);
        else if (const char* raw = archive_entry_pathname(entry)) entry_path = Widen(raw);
        std::replace(entry_path.begin(), entry_path.end(), L'\\', L'/');
        const auto slash = entry_path.find_last_of(L'/');
        const std::wstring folder = slash == std::wstring::npos ? std::wstring() : Lower(entry_path.substr(0, slash));
        const std::wstring name = SafeFileName(slash == std::wstring::npos ? entry_path : entry_path.substr(slash + 1));
        if (name.empty() || !HasExtension(name, L".vpk")) { ++result.SkippedEntries; continue; }
        if (archive_entry_is_encrypted(entry)) throw InvalidData("The archive is password protected.");
        if (archive_entry_size_is_set(entry) && static_cast<uint64_t>(archive_entry_size(entry)) > limits.MaxEntryBytes)
            throw InvalidData("A file in the archive is too large.");

        const auto [found, inserted] = folders.emplace(folder, folders.size() + 1);
        const auto directory = destination / std::to_wstring(found->second);
        if (inserted) fs::create_directories(directory);
        const auto target = directory / name;
        if (fs::exists(target, error)) { ++result.SkippedEntries; continue; }  // The same name twice, differing only in case.

        uint64_t size = 0;
        {
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) throw IoError("Could not write " + Narrow(target.wstring()) + ".");
            for (;;) {
                cancellation.ThrowIfCancelled();
                const la_ssize_t read = archive_read_data(reader.Handle, buffer.data(), buffer.size());
                if (read == 0) break;
                if (read < 0) reader.Fail("Could not extract " + Narrow(name));
                size += static_cast<uint64_t>(read);
                total += static_cast<uint64_t>(read);
                if (size > limits.MaxEntryBytes) throw InvalidData("A file in the archive is too large.");
                if (total > limits.MaxTotalBytes) throw InvalidData("The archive expands to more data than a mod should.");
                out.write(buffer.data(), read);
                if (!out) throw IoError("Could not write " + Narrow(target.wstring()) + ". The disk may be full.");
                if (progress) progress(total);
            }
        }
    }

    for (size_t index = 1; index <= folders.size(); ++index) RenameGameLikeChunks(destination / std::to_wstring(index));
    // Archive order, with each folder's chunk files left out: VpkArchive opens them through their _dir.vpk.
    for (size_t index = 1; index <= folders.size(); ++index) {
        const auto directory = destination / std::to_wstring(index);
        std::vector<fs::path> files;
        for (const auto& file : fs::directory_iterator(directory)) if (file.is_regular_file()) files.push_back(file.path());
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return CompareIgnoreCase(Narrow(a.filename().wstring()), Narrow(b.filename().wstring())) < 0;
        });
        for (const auto& file : files) if (!IsChunkName(file.filename().wstring())) result.Archives.push_back(file);
    }
    if (result.Archives.empty())
        throw InvalidData("The download has no mod VPK. It may need to be installed by hand; check the mod page for instructions.");
    return result;
}

}  // namespace skeleton_repair
