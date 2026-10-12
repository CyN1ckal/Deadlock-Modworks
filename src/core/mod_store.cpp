#include "mod_store.hpp"
#include "content_hash.hpp"
#include "content_store.hpp"
#include "strings.hpp"
#include "error.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <sqlite3.h>
#include <zstd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <cstdio>
#include <limits>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using Hash = std::array<uint8_t, 32>;
// "DMM1": identifies a shared library file, so arbitrary SQLite files are not merged.
constexpr int kApplicationId = 0x444D4D31;
constexpr int kSharedSchemaVersion = 1;

std::string Hex(const uint8_t* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
bool IsContentId(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string RandomId() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw IoError("Could not generate a temporary file name.");
    return Hex(bytes.data(), bytes.size());
}
std::string Key(const fs::path& path) {
    std::wstring name = fs::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    return Narrow(name);
}
bool SamePath(const fs::path& a, const fs::path& b) { return Key(a) == Key(b); }
int64_t Stamp(const fs::path& path) { return fs::last_write_time(path).time_since_epoch().count(); }
int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) Fail();
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& Bind(int index, int64_t value) { Check(sqlite3_bind_int64(statement_, index, value)); return *this; }
    Statement& Bind(int index, const std::string& value) {
        Check(sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT)); return *this;
    }
    Statement& Bind(int index, const uint8_t* data, size_t size) {
        Check(sqlite3_bind_blob64(statement_, index, size ? data : reinterpret_cast<const uint8_t*>(""), size, SQLITE_TRANSIENT)); return *this;
    }
    Statement& Bind(int index, const Hash& value) { return Bind(index, value.data(), value.size()); }
    Statement& Bind(int index, const std::vector<uint8_t>& value) { return Bind(index, value.data(), value.size()); }
    // True while a row is available.
    bool Step() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        Fail();
    }
    void Run() { Step(); Reset(); }
    void Reset() { sqlite3_reset(statement_); sqlite3_clear_bindings(statement_); }
    int64_t Int(int column) const { return sqlite3_column_int64(statement_, column); }
    std::string Text(int column) const {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
        return text ? std::string(text, static_cast<size_t>(sqlite3_column_bytes(statement_, column))) : std::string();
    }
    std::vector<uint8_t> Blob(int column) const {
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(statement_, column));
        return data ? std::vector<uint8_t>(data, data + sqlite3_column_bytes(statement_, column)) : std::vector<uint8_t>();
    }
    Hash HashColumn(int column) const {
        const auto bytes = Blob(column);
        if (bytes.size() != 32) throw InvalidData("The library database contains an invalid checksum.");
        Hash result{}; std::copy(bytes.begin(), bytes.end(), result.begin());
        return result;
    }
private:
    void Check(int result) { if (result != SQLITE_OK) Fail(); }
    [[noreturn]] void Fail() {
        if (sqlite3_errcode(db_) == SQLITE_INTERRUPT) throw OperationCancelled();
        throw IoError(std::string("Library database error: ") + sqlite3_errmsg(db_));
    }
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

class SqliteDb {
public:
    SqliteDb(const fs::path& path, int flags) {
        const auto name = Narrow(path.wstring());
        if (sqlite3_open_v2(name.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            throw IoError("Could not open the library database " + name + ": " + message);
        }
        sqlite3_busy_timeout(db_, 60000);
    }
    ~SqliteDb() {
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        sqlite3_close(db_);
    }
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    sqlite3* Get() const { return db_; }
    void Exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            const bool cancelled = sqlite3_errcode(db_) == SQLITE_INTERRUPT;
            sqlite3_free(error);
            if (cancelled) throw OperationCancelled();
            throw IoError("Library database error: " + message);
        }
    }
    int64_t Value(const char* sql) {
        Statement statement(db_, sql);
        return statement.Step() ? statement.Int(0) : 0;
    }
    // Long statements stop with OperationCancelled once the token is cancelled.
    void Cancel(CancellationToken token) {
        token_ = std::move(token);
        sqlite3_progress_handler(db_, 10000, [](void* self) { return static_cast<SqliteDb*>(self)->token_.IsCancelled() ? 1 : 0; }, this);
    }
private:
    sqlite3* db_ = nullptr;
    CancellationToken token_;
};

// Rolls back unless committed.
class Transaction {
public:
    explicit Transaction(SqliteDb& db) : db_(db) { db_.Exec("BEGIN IMMEDIATE"); }
    ~Transaction() { if (open_) sqlite3_exec(db_.Get(), "ROLLBACK", nullptr, nullptr, nullptr); }
    void Commit() { db_.Exec("COMMIT"); open_ = false; }
private:
    SqliteDb& db_;
    bool open_ = true;
};

// The gallery's own tables, next to the ContentStore tables in catalog.sqlite. Mods are ContentStore snapshots.
class GalleryDb : public SqliteDb {
public:
    explicit GalleryDb(const fs::path& catalog) : SqliteDb(Prepare(catalog), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
        Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        if (Value("SELECT count(*) FROM sqlite_schema WHERE name = 'gallery_settings'") != 0) return;
        Transaction transaction(*this);
        Exec(R"sql(
CREATE TABLE IF NOT EXISTS gallery_settings(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
-- The file name of each mod's rebuilt copy, from the mod's name when the library was last loaded.
CREATE TABLE IF NOT EXISTS gallery_names(snapshot_id TEXT PRIMARY KEY, name TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS gallery_sources(position INTEGER PRIMARY KEY, snapshot_id TEXT NOT NULL UNIQUE);
CREATE TABLE IF NOT EXISTS gallery_selected(
    position INTEGER PRIMARY KEY,
    snapshot_id TEXT NOT NULL,
    model TEXT NOT NULL,
    UNIQUE(snapshot_id, model)
);
-- Copies from the folder-based store of earlier versions, by lowercase path. Not shared with other libraries.
CREATE TABLE IF NOT EXISTS gallery_legacy_paths(
    path TEXT PRIMARY KEY,
    snapshot_id TEXT NOT NULL,
    size INTEGER NOT NULL,
    stamp INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS thumbnails(key TEXT PRIMARY KEY, png BLOB NOT NULL) WITHOUT ROWID;
)sql");
        transaction.Commit();
    }
    std::optional<std::string> Setting(const std::string& key) {
        Statement select(Get(), "SELECT value FROM gallery_settings WHERE key = ?");
        select.Bind(1, key);
        if (!select.Step()) return std::nullopt;
        return select.Text(0);
    }
    void SetSetting(const std::string& key, const std::string& value) {
        Statement(Get(), "INSERT INTO gallery_settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value")
            .Bind(1, key).Bind(2, value).Run();
    }
private:
    static const fs::path& Prepare(const fs::path& catalog) {
        fs::create_directories(catalog.parent_path());
        return catalog;
    }
};

// A standalone library file for sharing: mods, collection and thumbnails in one SQLite file.
// Its format is unchanged from the release that first shared libraries, so files from that release can be added.
constexpr const char* kSharedSchema = R"sql(
-- One row per distinct file content. `compression` is 0 for raw data and 1 for zstd.
CREATE TABLE blobs(
    hash BLOB PRIMARY KEY CHECK(length(hash) = 32),
    size INTEGER NOT NULL,
    crc INTEGER NOT NULL,
    compression INTEGER NOT NULL,
    data BLOB NOT NULL
) WITHOUT ROWID;
-- One row per distinct mod. content_hash covers every path and file checksum, so identical mods share a row.
CREATE TABLE archives(
    id INTEGER PRIMARY KEY,
    content_hash BLOB NOT NULL UNIQUE CHECK(length(content_hash) = 32),
    name TEXT NOT NULL,
    imported_at INTEGER NOT NULL
);
CREATE TABLE archive_entries(
    archive INTEGER NOT NULL REFERENCES archives(id),
    path TEXT NOT NULL,
    blob BLOB NOT NULL REFERENCES blobs(hash),
    PRIMARY KEY(archive, path)
) WITHOUT ROWID;
CREATE INDEX archive_entries_blob ON archive_entries(blob);
CREATE TABLE archive_origins(
    archive INTEGER NOT NULL REFERENCES archives(id),
    path TEXT NOT NULL,
    size INTEGER NOT NULL,
    stamp INTEGER NOT NULL,
    chunks TEXT NOT NULL,
    PRIMARY KEY(path, size, stamp, chunks)
) WITHOUT ROWID;
CREATE TABLE legacy_paths(
    path TEXT PRIMARY KEY,
    archive INTEGER NOT NULL REFERENCES archives(id),
    size INTEGER NOT NULL,
    stamp INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE library_sources(
    position INTEGER PRIMARY KEY,
    archive INTEGER NOT NULL UNIQUE REFERENCES archives(id)
);
CREATE TABLE library_selected(
    position INTEGER PRIMARY KEY,
    archive INTEGER NOT NULL REFERENCES archives(id),
    model TEXT NOT NULL,
    UNIQUE(archive, model)
);
CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
CREATE TABLE thumbnails(key TEXT PRIMARY KEY, png BLOB NOT NULL) WITHOUT ROWID;
PRAGMA application_id = 1145916721;
PRAGMA user_version = 1;
)sql";
static_assert(kApplicationId == 1145916721, "Update the shared schema script's application_id.");

struct StoredFile { std::string Path; Hash Blob; };

// The shared file's identifier for a mod: every path with its file checksum.
Hash SharedContentHash(std::vector<StoredFile> files) {
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.Path < b.Path; });
    std::vector<uint8_t> manifest;
    for (const auto& file : files) {
        manifest.insert(manifest.end(), file.Path.begin(), file.Path.end());
        manifest.push_back(0);
        manifest.insert(manifest.end(), file.Blob.begin(), file.Blob.end());
    }
    return Sha256(manifest);
}

// Stored names come from imported files or shared libraries, so only a plain .vpk file name is used.
std::wstring SafeName(const std::string& name) {
    const auto wide = fs::path(Widen(name)).filename().wstring();
    const bool valid = !wide.empty() && wide.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos
        && EqualsIgnoreCase(Narrow(fs::path(wide).extension().wstring()), ".vpk")
        && !EqualsIgnoreCase(Narrow(wide), "pak01_dir.vpk");
    return valid ? wide : L"mod.vpk";
}

// The rebuilt copy's file name: the mod's name made safe for Windows, and never a name the library refuses to import.
std::wstring FileName(const std::string& label) {
    std::wstring name = Widen(label);
    for (auto& c : name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
    constexpr size_t kLimit = 100;
    if (name.size() > kLimit) name.resize(IS_HIGH_SURROGATE(name[kLimit - 1]) ? kLimit - 1 : kLimit);
    const auto trim = name.find_last_not_of(L" .");
    name.erase(trim == std::wstring::npos ? 0 : trim + 1);
    name.erase(0, std::min(name.find_first_not_of(L' '), name.size()));
    if (name.empty()) name = L"mod";
    std::wstring upper = name;
    CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size()));
    const bool device = upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL"
        || (upper.size() == 4 && (upper.starts_with(L"COM") || upper.starts_with(L"LPT")) && upper[3] >= L'1' && upper[3] <= L'9');
    // The library refuses the game archive and chunk files such as name_000.vpk.
    const bool chunk = name.size() >= 4 && name[name.size() - 4] == L'_'
        && std::all_of(name.end() - 3, name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (device || chunk || upper == L"PAK01_DIR") name += L'_';
    return name + L".vpk";
}

// Removes a temporary folder when it goes out of scope.
struct TemporaryFolder {
    fs::path Path;
    ~TemporaryFolder() { std::error_code error; fs::remove_all(Path, error); }
};

struct Context {
    const fs::path& Root;
    ContentStore& Content;
    GalleryDb& Db;
    fs::path Cache() const { return Root / L"cache"; }
    std::string Name(const SnapshotId& snapshot) const {
        const auto label = Content.SnapshotLabel(snapshot);
        return label.empty() ? "mod" : label;
    }
    // Named after the mod, so the gallery shows its name. The name only changes when `refresh` is set, which Load does,
    // so a path handed out stays valid until the library is loaded again, even if a later import changes the mod's name.
    fs::path CachePath(const std::string& content_id, const SnapshotId& snapshot, bool refresh = false) const {
        std::string stored;
        {
            Statement select(Db.Get(), "SELECT name FROM gallery_names WHERE snapshot_id = ?");
            select.Bind(1, snapshot.Value);
            if (select.Step()) stored = select.Text(0);
        }
        auto name = Widen(stored);
        if (stored.empty() || refresh) {
            name = FileName(Name(snapshot));
            if (Narrow(name) != stored)
                Statement(Db.Get(), "INSERT OR REPLACE INTO gallery_names(snapshot_id, name) VALUES(?, ?)").Bind(1, snapshot.Value).Bind(2, Narrow(name)).Run();
        }
        return Cache() / Widen(content_id) / name;
    }
    static std::string StampText(const fs::path& path) {
        return std::to_string(fs::file_size(path)) + " " + std::to_string(Stamp(path));
    }
    // True when the rebuilt copy is still exactly what the store wrote, for example not repaired in place since.
    static bool Fresh(const fs::path& path) {
        std::error_code error;
        if (!fs::is_regular_file(path, error)) return false;
        std::ifstream in(path.parent_path() / L"stamp", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        try { return text == StampText(path); } catch (const std::exception&) { return false; }
    }
    std::optional<std::string> CacheContent(const fs::path& path) const {
        const auto full = fs::absolute(path).lexically_normal();
        if (!SamePath(full.parent_path().parent_path(), Cache())) return std::nullopt;
        const auto id = Narrow(full.parent_path().filename().wstring());
        return IsContentId(id) ? std::optional(id) : std::nullopt;
    }
    std::optional<SnapshotId> SnapshotFor(const fs::path& path) const {
        const auto content = CacheContent(path);
        return content ? Content.FindSnapshot(*content) : std::nullopt;
    }
    std::string ContentIdOf(const SnapshotId& snapshot) const {
        const auto id = Content.SnapshotContentId(snapshot);
        if (!id) throw InvalidData("This mod is no longer in the model library.");
        return *id;
    }
    // Writes the snapshot's archive from the library, unless an unchanged copy is already there.
    fs::path Materialize(const SnapshotId& snapshot, CancellationToken cancellation, bool refresh = false) const {
        const auto destination = CachePath(ContentIdOf(snapshot), snapshot, refresh);
        if (Fresh(destination)) return destination;
        fs::create_directories(destination.parent_path());
        // A copy written under an earlier name of the mod is renamed rather than rebuilt. Renaming keeps its stamp.
        std::error_code listing;
        for (const auto& entry : fs::directory_iterator(destination.parent_path(), listing)) {
            const auto& old = entry.path();
            if (!EqualsIgnoreCase(Narrow(old.extension().wstring()), ".vpk") || SamePath(old, destination) || !Fresh(old)) continue;
            if (MoveFileExW(old.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING) && Fresh(destination)) return destination;
            break;
        }
        const auto temp = destination.parent_path() / Widen("build-" + RandomId() + ".tmp");
        try {
            std::vector<VpkNewFile> files;
            for (const auto& member : Content.SnapshotMembers(snapshot)) {
                if (member.Size < 0 || member.Size > std::numeric_limits<int32_t>::max()) throw InvalidData("The model library contains an invalid file size.");
                files.push_back({member.Path, member.Crc32, static_cast<uint32_t>(member.Size),
                    [this, blob = member.Blob](const std::function<void(const uint8_t*, size_t)>& sink) {
                        // ReadBlob checks the file against its checksum.
                        const auto data = Content.ReadBlob(blob);
                        sink(data.data(), data.size());
                    }});
            }
            VpkArchive::Create(temp, files, cancellation);
            VpkArchive::Open(temp);
            if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw IoError("Could not rebuild " + Narrow(destination.wstring()) + ". Windows error " + std::to_string(GetLastError()));
            std::ofstream(destination.parent_path() / L"stamp", std::ios::binary | std::ios::trunc) << StampText(destination);
        } catch (...) {
            std::error_code error;
            fs::remove(temp, error);
            throw;
        }
        return destination;
    }
    SnapshotId SourceSnapshot(const fs::path& source, CancellationToken cancellation) const {
        const auto full = fs::absolute(source).lexically_normal();
        if (!EqualsIgnoreCase(Narrow(full.extension().wstring()), ".vpk")) throw InvalidData("Import requires a .vpk archive.");
        if (Fresh(full)) if (const auto snapshot = SnapshotFor(full)) return *snapshot;
        // A rebuilt copy changed outside the store, for example by a repair, is imported as a new version.
        const auto previous = SnapshotFor(full);
        const auto snapshot = Content.ImportVpk(full, cancellation);
        // The new version keeps the name of the one it was changed from.
        if (previous && previous->Value != snapshot.Value && !Content.SnapshotName(snapshot)) {
            const auto label = Content.SnapshotLabel(*previous);
            if (!label.empty() && label != previous->Value) Content.RenameSnapshot(snapshot, label);
        }
        return snapshot;
    }
    std::vector<SnapshotId> Sources() const {
        Statement select(Db.Get(), "SELECT snapshot_id FROM gallery_sources ORDER BY position");
        std::vector<SnapshotId> result;
        while (select.Step()) result.push_back({select.Text(0)});
        return result;
    }
    std::vector<std::pair<std::string, std::string>> Selected() const {
        Statement select(Db.Get(), "SELECT snapshot_id, model FROM gallery_selected ORDER BY position");
        std::vector<std::pair<std::string, std::string>> result;
        while (select.Step()) result.emplace_back(select.Text(0), select.Text(1));
        return result;
    }
    // Appends sources and selections that are not in the collection yet. Existing entries keep their order.
    void AddToCollection(const std::vector<SnapshotId>& sources, const std::vector<std::pair<std::string, std::string>>& selected) const {
        Statement source(Db.Get(), "INSERT OR IGNORE INTO gallery_sources(position, snapshot_id) VALUES((SELECT coalesce(max(position), 0) + 1 FROM gallery_sources), ?)");
        for (const auto& id : sources) source.Bind(1, id.Value).Run();
        Statement selection(Db.Get(), "INSERT OR IGNORE INTO gallery_selected(position, snapshot_id, model) VALUES((SELECT coalesce(max(position), 0) + 1 FROM gallery_selected), ?, ?)");
        for (const auto& [id, model] : selected) selection.Bind(1, id).Bind(2, model).Run();
    }
    void SetGameIfUnset(const fs::path& game) const {
        if (game.empty() || Db.Setting("game_archive")) return;
        Db.SetSetting("game_archive", Narrow(fs::absolute(game).lexically_normal().wstring()));
    }

    void MergeShared(const fs::path& other, CancellationToken cancellation) const;

    // Merges the single-file library.db of the previous release once. The file is only read.
    void MergeSingleFileStore(CancellationToken cancellation, std::string& notice) const {
        const auto previous = Root / L"library.db";
        if (Db.Setting("library_db_merged") || !fs::exists(previous)) return;
        try {
            MergeShared(previous, cancellation);
            Db.SetSetting("library_db_merged", "1");
        } catch (const OperationCancelled&) { throw; }
        catch (const std::exception& failure) {
            notice += "Your library from the previous version could not be added: " + std::string(failure.what()) + " It is still in " + Narrow(previous.wstring()) + ".\n";
        }
    }

    // Merges the folder-based store of earlier versions: every stored VPK, the collection and the selections.
    // The old files are only read, never moved or removed.
    void MergeLegacy(CancellationToken cancellation, const StoreProgress& progress, std::string& notice) const {
        if (Db.Setting("legacy_store_merged")) return;
        const auto mods = Root / L"mods";
        std::vector<fs::path> archives;
        std::error_code error;
        if (fs::is_directory(mods, error)) {
            for (const auto& folder : fs::directory_iterator(mods)) {
                if (!folder.is_directory()) continue;
                for (const auto& file : fs::directory_iterator(folder.path()))
                    if (file.is_regular_file() && EqualsIgnoreCase(Narrow(file.path().extension().wstring()), ".vpk")) archives.push_back(file.path());
            }
        }
        std::sort(archives.begin(), archives.end());
        const auto main = Root / L"library.json", backup = Root / L"library.backup.json";
        if (archives.empty() && !fs::exists(main) && !fs::exists(backup)) { Db.SetSetting("legacy_store_merged", "1"); return; }

        std::map<std::string, SnapshotId> imported;
        Statement known(Db.Get(), "SELECT snapshot_id FROM gallery_legacy_paths WHERE path = ? AND size = ? AND stamp = ?");
        std::string problems;
        for (size_t i = 0; i < archives.size(); ++i) {
            cancellation.ThrowIfCancelled();
            if (progress) progress(i, archives.size());
            const auto& path = archives[i];
            try {
                const auto size = static_cast<int64_t>(fs::file_size(path));
                const auto stamp = Stamp(path);
                known.Bind(1, Key(path)).Bind(2, size).Bind(3, stamp);
                std::optional<SnapshotId> id;
                if (known.Step()) id = SnapshotId{known.Text(0)};
                known.Reset();
                if (!id || !Content.SnapshotContentId(*id)) {
                    id = Content.ImportVpk(path, cancellation);
                    Statement(Db.Get(), "INSERT OR REPLACE INTO gallery_legacy_paths(path, snapshot_id, size, stamp) VALUES(?, ?, ?, ?)")
                        .Bind(1, Key(path)).Bind(2, id->Value).Bind(3, size).Bind(4, stamp).Run();
                }
                imported.emplace(Key(path), *id);
            } catch (const OperationCancelled&) { throw; }
            catch (const std::exception& failure) { problems += Narrow(path.wstring()) + ": " + failure.what() + "\n"; }
        }
        if (progress) progress(archives.size(), archives.size());

        std::optional<ModelLibrary> library;
        for (const auto& manifest : {main, backup}) {
            if (library || !fs::exists(manifest)) continue;
            try { library = ModelLibrary::Load(manifest); } catch (const std::exception&) {}
        }
        if (!library && (fs::exists(main) || fs::exists(backup)) && !Db.Setting("legacy_collection_merged"))
            problems += "The earlier collection list could not be read, so its mods were added without restoring the collection. It is still in " + Narrow(Root.wstring()) + ".\n";
        // The collection is merged once, so sources removed afterwards do not come back on the next start.
        if (library && !Db.Setting("legacy_collection_merged")) {
            std::map<std::string, SnapshotId> sources;
            for (const auto& source : library->Sources) {
                cancellation.ThrowIfCancelled();
                auto found = imported.find(Key(source));
                if (found != imported.end()) { sources.emplace(Key(source), found->second); continue; }
                // Sources outside the old store were used in place; store them now if they still exist.
                try { sources.emplace(Key(source), SourceSnapshot(source, cancellation)); }
                catch (const OperationCancelled&) { throw; }
                catch (const std::exception& failure) { problems += Narrow(source.wstring()) + ": " + failure.what() + "\n"; }
            }
            std::vector<SnapshotId> order;
            for (const auto& source : library->Sources) if (const auto found = sources.find(Key(source)); found != sources.end()) order.push_back(found->second);
            std::vector<std::pair<std::string, std::string>> selected;
            for (const auto& model : library->Selected)
                if (const auto found = sources.find(Key(model.Archive)); found != sources.end()) selected.emplace_back(found->second.Value, model.Model);
            Transaction transaction(Db);
            AddToCollection(order, selected);
            SetGameIfUnset(library->GameArchive);
            Db.SetSetting("legacy_collection_merged", "1");
            transaction.Commit();
        }
        if (problems.empty()) Db.SetSetting("legacy_store_merged", "1");
        notice += problems;
    }
};

// Checks a shared file's identity and version without changing it.
void CheckSharedFile(const fs::path& path) {
    sqlite3* check = nullptr;
    const auto name = Narrow(path.wstring());
    const int opened = sqlite3_open_v2(name.c_str(), &check, SQLITE_OPEN_READONLY, nullptr);
    int application = 0, version = 0;
    if (opened == SQLITE_OK) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(check, "PRAGMA application_id", -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) application = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        if (sqlite3_prepare_v2(check, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW) version = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
    }
    sqlite3_close(check);
    if (application != kApplicationId || version < 1) throw InvalidData(Narrow(path.filename().wstring()) + " is not a Deadlock mod library.");
    if (version > kSharedSchemaVersion) throw InvalidData(Narrow(path.filename().wstring()) + " was written by a newer version of the mod manager.");
}

std::vector<StoredFile> SharedFiles(SqliteDb& db, int64_t archive) {
    Statement select(db.Get(), "SELECT path, blob FROM archive_entries WHERE archive = ?");
    select.Bind(1, archive);
    std::vector<StoredFile> files;
    while (select.Step()) files.push_back({select.Text(0), select.HashColumn(1)});
    return files;
}

void Context::MergeShared(const fs::path& other, CancellationToken cancellation) const {
    if (!fs::is_regular_file(other)) throw IoError("Could not find " + Narrow(other.wstring()) + ".");
    CheckSharedFile(other);
    SqliteDb shared(other, SQLITE_OPEN_READONLY);
    shared.Cancel(cancellation);
    struct SharedArchive { int64_t Id = 0; std::string Name; };
    std::vector<SharedArchive> archives;
    {
        Statement select(shared.Get(), "SELECT id, content_hash, name FROM archives ORDER BY id");
        while (select.Step()) {
            cancellation.ThrowIfCancelled();
            // Content identifiers come from another file, so each mod must match the files it lists before any is added.
            if (SharedContentHash(SharedFiles(shared, select.Int(0))) != select.HashColumn(1))
                throw InvalidData(Narrow(other.filename().wstring()) + " is damaged: a stored mod does not match its checksum.");
            archives.push_back({select.Int(0), select.Text(2)});
        }
    }
    // Each mod is rebuilt as a VPK and imported like any other, so the model library indexes its models.
    const TemporaryFolder work{Root / Widen("merging-" + RandomId())};
    std::map<int64_t, SnapshotId> snapshots;
    Statement entries(shared.Get(), "SELECT e.path, b.hash, b.size, b.crc FROM archive_entries e JOIN blobs b ON b.hash = e.blob WHERE e.archive = ? ORDER BY e.path");
    Statement read(shared.Get(), "SELECT compression, data, size FROM blobs WHERE hash = ?");
    for (const auto& archive : archives) {
        cancellation.ThrowIfCancelled();
        const auto folder = work.Path / std::to_wstring(archive.Id);
        fs::create_directories(folder);
        const auto vpk = folder / SafeName(archive.Name);
        std::vector<VpkNewFile> files;
        entries.Bind(1, archive.Id);
        while (entries.Step()) {
            const auto size = entries.Int(2);
            if (size < 0 || size > std::numeric_limits<int32_t>::max()) throw InvalidData("The shared library contains an invalid file size.");
            files.push_back({entries.Text(0), static_cast<uint32_t>(entries.Int(3)), static_cast<uint32_t>(size),
                [&read, hash = entries.HashColumn(1), path = entries.Text(0)](const std::function<void(const uint8_t*, size_t)>& sink) {
                    read.Bind(1, hash);
                    if (!read.Step()) throw InvalidData("The shared library is missing " + path + ".");
                    auto data = read.Blob(1);
                    const auto compression = read.Int(0);
                    const auto size = static_cast<size_t>(read.Int(2));
                    read.Reset();
                    if (compression == 1) {
                        std::vector<uint8_t> raw(size);
                        const size_t written = ZSTD_decompress(raw.data(), raw.size(), data.data(), data.size());
                        if (ZSTD_isError(written) || written != size) throw InvalidData("Stored file " + path + " is damaged.");
                        data = std::move(raw);
                    } else if (compression != 0) {
                        throw InvalidData("Stored file " + path + " uses an unknown compression.");
                    }
                    if (data.size() != size || Sha256(data) != hash) throw InvalidData("Stored file " + path + " is damaged.");
                    sink(data.data(), data.size());
                }});
        }
        entries.Reset();
        VpkArchive::Create(vpk, files, cancellation);
        // Imported under the shared name, which the mod's label is derived from.
        snapshots.emplace(archive.Id, Content.ImportVpk(vpk, cancellation));
        std::error_code error;
        fs::remove_all(folder, error);
    }
    std::vector<SnapshotId> sources;
    Statement other_sources(shared.Get(), "SELECT archive FROM library_sources ORDER BY position");
    while (other_sources.Step()) if (const auto found = snapshots.find(other_sources.Int(0)); found != snapshots.end()) sources.push_back(found->second);
    std::vector<std::pair<std::string, std::string>> selected;
    Statement other_selected(shared.Get(), "SELECT archive, model FROM library_selected ORDER BY position");
    while (other_selected.Step()) if (const auto found = snapshots.find(other_selected.Int(0)); found != snapshots.end())
        selected.emplace_back(found->second.Value, other_selected.Text(1));
    Transaction transaction(Db);
    AddToCollection(sources, selected);
    Statement thumbnails(shared.Get(), "SELECT key, png FROM thumbnails");
    Statement insert(Db.Get(), "INSERT OR IGNORE INTO thumbnails(key, png) VALUES(?, ?)");
    while (thumbnails.Step()) insert.Bind(1, thumbnails.Text(0)).Bind(2, thumbnails.Blob(1)).Run();
    Statement other_game(shared.Get(), "SELECT value FROM settings WHERE key = 'game_archive'");
    if (other_game.Step()) {
        const fs::path game = Widen(other_game.Text(0));
        std::error_code error;
        if (fs::is_regular_file(game, error)) SetGameIfUnset(game);
    }
    transaction.Commit();
}

// Opens the content library and the gallery tables beside it.
struct Opened {
    explicit Opened(const LocalModStore& store, CancellationToken cancellation = {})
        : Content(ContentStore::Open(store.ContentDirectory())), Db(store.Database()) {
        Db.Cancel(std::move(cancellation));
    }
    ContentStore Content;
    GalleryDb Db;
};
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
    Opened opened(*this, cancellation);
    const Context context{root_, opened.Content, opened.Db};
    return context.Materialize(context.SourceSnapshot(source, cancellation), cancellation);
}

std::optional<std::string> LocalModStore::Name(const fs::path& archive) const {
    Opened opened(*this);
    const Context context{root_, opened.Content, opened.Db};
    const auto snapshot = context.SnapshotFor(archive);
    if (!snapshot) return std::nullopt;
    return context.Name(*snapshot);
}

void LocalModStore::Rename(const fs::path& archive, const std::string& name) const {
    Opened opened(*this);
    const Context context{root_, opened.Content, opened.Db};
    const auto snapshot = context.SnapshotFor(archive);
    if (!snapshot) throw InvalidData(Narrow(archive.wstring()) + " is not stored in the library.");
    opened.Content.RenameSnapshot(*snapshot, name);
}

ModelLibrary LocalModStore::Load(CancellationToken cancellation, const StoreProgress& progress, std::string* notice) const {
    Opened opened(*this, cancellation);
    const Context context{root_, opened.Content, opened.Db};
    std::string problems;
    context.MergeSingleFileStore(cancellation, problems);
    context.MergeLegacy(cancellation, progress, problems);
    ModelLibrary result;
    std::map<std::string, fs::path> paths;
    const auto sources = context.Sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        cancellation.ThrowIfCancelled();
        if (progress) progress(i, sources.size());
        const auto content_id = opened.Content.SnapshotContentId(sources[i]);
        if (!content_id) {
            // Hidden from the version history, which removes its files from the library.
            problems += context.Name(sources[i]) + " is no longer in the model library, so it was left out.\n";
            continue;
        }
        // Every path is handed out again here, so each copy takes the mod's current name.
        try { paths[sources[i].Value] = context.Materialize(sources[i], cancellation, true); }
        catch (const OperationCancelled&) { throw; }
        catch (const std::exception& error) {
            // Keep the source in the collection; the gallery reports that it cannot be opened.
            paths[sources[i].Value] = context.CachePath(*content_id, sources[i]);
            problems += context.Name(sources[i]) + ": " + error.what() + "\n";
        }
        result.Sources.push_back(paths[sources[i].Value]);
    }
    for (const auto& [id, model] : context.Selected())
        if (const auto found = paths.find(id); found != paths.end()) result.Selected.push_back({found->second, model});
    if (const auto game = opened.Db.Setting("game_archive")) {
        const fs::path path = Widen(*game);
        // A library shared from another computer keeps its game path, which only applies where it exists.
        std::error_code error;
        if (fs::is_regular_file(path, error)) result.GameArchive = path;
    }
    if (notice) *notice = problems;
    return result;
}

void LocalModStore::Save(const ModelLibrary& library) const {
    Opened opened(*this);
    const Context context{root_, opened.Content, opened.Db};
    std::vector<SnapshotId> sources;
    std::map<std::string, std::string> ids;
    for (const auto& source : library.Sources) {
        const auto id = context.SnapshotFor(source);
        if (!id) throw InvalidData(Narrow(source.wstring()) + " is not stored in the library.");
        ids[Key(source)] = id->Value;
        if (std::none_of(sources.begin(), sources.end(), [&](const auto& other) { return other.Value == id->Value; })) sources.push_back(*id);
    }
    std::vector<std::pair<std::string, std::string>> selected;
    for (const auto& model : library.Selected) {
        const auto found = ids.find(Key(model.Archive));
        if (found == ids.end()) throw InvalidData("Selected model is not in the library sources.");
        selected.emplace_back(found->second, VpkArchive::Normalize(model.Model));
    }
    Transaction transaction(opened.Db);
    opened.Db.Exec("DELETE FROM gallery_sources; DELETE FROM gallery_selected;");
    context.AddToCollection(sources, selected);
    if (!library.GameArchive.empty()) opened.Db.SetSetting("game_archive", Narrow(fs::absolute(library.GameArchive).lexically_normal().wstring()));
    transaction.Commit();
}

void LocalModStore::Merge(const fs::path& other, CancellationToken cancellation) const {
    if (SamePath(other, Database())) throw InvalidData("Choose a library other than the one already open.");
    Opened opened(*this, cancellation);
    const Context context{root_, opened.Content, opened.Db};
    context.MergeShared(other, cancellation);
}

void LocalModStore::Export(const fs::path& destination, CancellationToken cancellation) const {
    if (SamePath(destination, Database())) throw InvalidData("Choose a location other than the library itself.");
    const auto full = fs::absolute(destination).lexically_normal();
    const auto temp = full.parent_path() / Widen("library-" + RandomId() + ".tmp");
    try {
        Opened opened(*this, cancellation);
        const Context context{root_, opened.Content, opened.Db};
        {
            SqliteDb shared(temp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
            shared.Cancel(cancellation);
            Transaction transaction(shared);
            shared.Exec(kSharedSchema);
            Statement exists(shared.Get(), "SELECT 1 FROM blobs WHERE hash = ?");
            Statement blob(shared.Get(), "INSERT INTO blobs(hash, size, crc, compression, data) VALUES(?, ?, ?, ?, ?)");
            Statement archive(shared.Get(), "INSERT OR IGNORE INTO archives(content_hash, name, imported_at) VALUES(?, ?, ?)");
            Statement find(shared.Get(), "SELECT id FROM archives WHERE content_hash = ?");
            Statement entry(shared.Get(), "INSERT OR IGNORE INTO archive_entries(archive, path, blob) VALUES(?, ?, ?)");
            std::map<std::string, int64_t> archives;
            for (const auto& pack : opened.Content.ListPacks()) {
                cancellation.ThrowIfCancelled();
                std::vector<StoredFile> files;
                const auto members = opened.Content.SnapshotMembers(pack.Snapshot);
                for (const auto& member : members) {
                    files.push_back({member.Path, member.Blob.Bytes});
                    exists.Bind(1, member.Blob.Bytes);
                    const bool stored = exists.Step();
                    exists.Reset();
                    if (stored) continue;
                    const auto bytes = opened.Content.ReadBlob(member.Blob);
                    std::vector<uint8_t> packed(ZSTD_compressBound(bytes.size()));
                    const size_t size = ZSTD_compress(packed.data(), packed.size(), bytes.data(), bytes.size(), 3);
                    // Many resources are already compressed, so keep them raw when zstd does not help.
                    const bool compressed = !ZSTD_isError(size) && size < bytes.size();
                    if (compressed) packed.resize(size);
                    blob.Bind(1, member.Blob.Bytes).Bind(2, static_cast<int64_t>(bytes.size())).Bind(3, static_cast<int64_t>(member.Crc32))
                        .Bind(4, compressed ? 1 : 0).Bind(5, compressed ? packed : bytes).Run();
                }
                const auto content = SharedContentHash(files);
                archive.Bind(1, content).Bind(2, Narrow(FileName(pack.Label))).Bind(3, Now()).Run();
                find.Bind(1, content);
                find.Step();
                const auto id = find.Int(0);
                find.Reset();
                archives.emplace(pack.Snapshot.Value, id);
                for (const auto& member : members) entry.Bind(1, id).Bind(2, member.Path).Bind(3, member.Blob.Bytes).Run();
            }
            Statement source(shared.Get(), "INSERT OR IGNORE INTO library_sources(position, archive) VALUES(?, ?)");
            int64_t position = 0;
            for (const auto& snapshot : context.Sources())
                if (const auto found = archives.find(snapshot.Value); found != archives.end()) source.Bind(1, ++position).Bind(2, found->second).Run();
            Statement selection(shared.Get(), "INSERT OR IGNORE INTO library_selected(position, archive, model) VALUES(?, ?, ?)");
            position = 0;
            for (const auto& [snapshot, model] : context.Selected())
                if (const auto found = archives.find(snapshot); found != archives.end()) selection.Bind(1, ++position).Bind(2, found->second).Bind(3, model).Run();
            Statement thumbnails(opened.Db.Get(), "SELECT key, png FROM thumbnails");
            Statement thumbnail(shared.Get(), "INSERT INTO thumbnails(key, png) VALUES(?, ?)");
            while (thumbnails.Step()) thumbnail.Bind(1, thumbnails.Text(0)).Bind(2, thumbnails.Blob(1)).Run();
            if (const auto game = opened.Db.Setting("game_archive"))
                Statement(shared.Get(), "INSERT INTO settings(key, value) VALUES('game_archive', ?)").Bind(1, *game).Run();
            transaction.Commit();
        }
        if (!MoveFileExW(temp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw IoError("Could not write " + Narrow(full.wstring()) + ". Windows error " + std::to_string(GetLastError()));
    } catch (...) {
        std::error_code error;
        fs::remove(temp, error);
        throw;
    }
}

std::optional<std::string> LocalModStore::ContentId(const fs::path& archive) const {
    const auto full = fs::absolute(archive).lexically_normal();
    if (!SamePath(full.parent_path().parent_path(), root_ / L"cache")) return std::nullopt;
    const auto id = Narrow(full.parent_path().filename().wstring());
    if (!IsContentId(id)) return std::nullopt;
    return id;
}

std::vector<fs::path> LocalModStore::LegacyPaths(const fs::path& archive) const {
    const auto id = ContentId(archive);
    if (!id) return {};
    Opened opened(*this);
    const auto snapshot = opened.Content.FindSnapshot(*id);
    if (!snapshot) return {};
    Statement select(opened.Db.Get(), "SELECT path FROM gallery_legacy_paths WHERE snapshot_id = ?");
    select.Bind(1, snapshot->Value);
    std::vector<fs::path> result;
    while (select.Step()) result.emplace_back(Widen(select.Text(0)));
    return result;
}

std::optional<std::vector<uint8_t>> LocalModStore::Thumbnail(const std::string& key) const {
    GalleryDb db(Database());
    Statement select(db.Get(), "SELECT png FROM thumbnails WHERE key = ?");
    select.Bind(1, key);
    if (!select.Step()) return std::nullopt;
    return select.Blob(0);
}

void LocalModStore::SaveThumbnail(const std::string& key, const std::vector<uint8_t>& png) const {
    GalleryDb db(Database());
    Statement(db.Get(), "INSERT OR REPLACE INTO thumbnails(key, png) VALUES(?, ?)").Bind(1, key).Bind(2, png).Run();
}
}
