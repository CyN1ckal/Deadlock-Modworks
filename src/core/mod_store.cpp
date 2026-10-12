#include "mod_store.hpp"
#include "archive_transaction.hpp"
#include "strings.hpp"
#include "error.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <set>
#include <sstream>
#include <iomanip>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
std::string Id() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw IoError("Could not generate a local mod identifier.");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}
bool SamePath(const fs::path& a, const fs::path& b) {
    return EqualsIgnoreCase(Narrow(fs::absolute(a).lexically_normal().wstring()), Narrow(fs::absolute(b).lexically_normal().wstring()));
}
bool Owned(const fs::path& path, const fs::path& folder) {
    auto root = Narrow(fs::weakly_canonical(folder).wstring());
    if (!root.ends_with("\\")) root += "\\";
    const auto full = Narrow(fs::weakly_canonical(path).wstring());
    return full.size() > root.size() && EqualsIgnoreCase(full.substr(0, root.size()), root);
}
void Publish(const fs::path& temp, const fs::path& destination) {
    if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw IoError("Could not save the local mod library. Windows error " + std::to_string(GetLastError()));
}
}
LocalModStore::LocalModStore(fs::path root) : root_(fs::absolute(std::move(root)).lexically_normal()) {}
fs::path LocalModStore::DefaultRoot() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        throw IoError("Could not find the local app data folder.");
    const fs::path result = fs::path(folder) / L"DeadlockModManager";
    CoTaskMemFree(folder);
    return result;
}
fs::path LocalModStore::Import(const fs::path& source, CancellationToken cancellation) const {
    cancellation.ThrowIfCancelled();
    const auto full = fs::absolute(source).lexically_normal();
    if (!EqualsIgnoreCase(Narrow(full.extension().wstring()), ".vpk")) throw InvalidData("Import requires a .vpk archive.");
    const auto mods = root_ / L"mods";
    if (Owned(full, mods) && fs::is_regular_file(full)) {
        VpkArchive::Open(full); // Existing local copies can be reopened without the original.
        return full;
    }
    const auto archive = VpkArchive::Open(full);
    auto lease = archive.LockSources();
    const auto size = fs::file_size(full);
    const auto stamp = fs::last_write_time(full).time_since_epoch().count();
    auto chunks = nlohmann::json::array();
    std::set<uint16_t> chunk_ids;
    for (const auto& entry : archive.Entries()) if (entry.ArchiveIndex != VpkArchive::EmbeddedArchive) chunk_ids.insert(entry.ArchiveIndex);
    auto basename = full.stem().wstring();
    if (basename.ends_with(L"_dir")) basename.resize(basename.size() - 4);
    for (const auto id : chunk_ids) {
        std::wostringstream name;
        name << basename << L"_" << std::setw(3) << std::setfill(L'0') << id << L".vpk";
        const auto chunk = full.parent_path() / name.str();
        chunks.push_back({{"index", id}, {"size", fs::file_size(chunk)}, {"stamp", fs::last_write_time(chunk).time_since_epoch().count()}});
    }
    fs::create_directories(mods);
    // Original path and file stamp identify repeat imports. Different versions get independent copies.
    for (const auto& entry : fs::directory_iterator(mods)) {
        cancellation.ThrowIfCancelled();
        if (!entry.is_directory()) continue;
        try {
            std::ifstream input(entry.path() / L"origin.json", std::ios::binary);
            if (!input) continue;
            nlohmann::json json; input >> json;
            if (!SamePath(fs::path(Widen(json.at("original").get<std::string>())), full)
                || json.at("size").get<uintmax_t>() != size || json.at("stamp").get<int64_t>() != stamp
                || json.value("chunks", nlohmann::json::array()) != chunks) continue;
            const auto cached = entry.path() / full.filename();
            if (!Owned(cached, mods) || !fs::is_regular_file(cached)) continue;
            VpkArchive::Open(cached);
            return cached;
        } catch (const std::exception&) { /* Ignore an incomplete or stale import record. */ }
    }
    const auto folder = mods / Widen(Id());
    fs::create_directory(folder);
    const auto cached = folder / full.filename();
    try {
        ArchiveTransaction::Write(archive, full, cached, {}, false, cancellation);
        cancellation.ThrowIfCancelled();
        const nlohmann::json metadata = {{"original", Narrow(full.wstring())}, {"size", size}, {"stamp", stamp}, {"chunks", chunks}};
        std::ofstream output(folder / L"origin.json", std::ios::binary);
        output << metadata.dump(2) << '\n'; output.flush();
        if (!output) throw IoError("Could not record the imported mod.");
        return cached;
    } catch (...) {
        // This is the fresh random-id folder created by this import, under the store.
        std::error_code error;
        if (Owned(folder, mods)) fs::remove_all(folder, error);
        throw;
    }
}
ModelLibrary LocalModStore::Load() const {
    const auto main = root_ / L"library.json";
    const auto backup = root_ / L"library.backup.json";
    if (!fs::exists(main) && !fs::exists(backup)) return {};
    try { return ModelLibrary::Load(main); }
    catch (const std::exception&) {
        try { return ModelLibrary::Load(backup); }
        catch (const std::exception&) { throw InvalidData("Could not restore the local library or its backup. Imported VPK copies are still in " + Narrow((root_ / L"mods").wstring())); }
    }
}
void LocalModStore::Save(const ModelLibrary& library) const {
    fs::create_directories(root_);
    const auto temp = root_ / Widen("library-" + Id() + ".json");
    const auto backup_temp = root_ / Widen("backup-" + Id() + ".json");
    const auto main = root_ / L"library.json";
    try {
        library.Save(temp);
        if (fs::exists(main)) {
            // Only a readable previous manifest is allowed to replace the recovery copy.
            bool valid = false;
            try { ModelLibrary::Load(main); valid = true; } catch (const std::exception&) {}
            if (valid) {
                fs::copy_file(main, backup_temp);
                Publish(backup_temp, root_ / L"library.backup.json");
            }
        }
        Publish(temp, main);
    } catch (...) {
        std::error_code error;
        fs::remove(temp, error); fs::remove(backup_temp, error);
        throw;
    }
}
}
