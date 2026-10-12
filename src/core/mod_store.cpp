#include "mod_store.hpp"
#include "strings.hpp"
#include "error.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <zstd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <limits>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using Hash = std::array<uint8_t, 32>;
// "DMM1": identifies a library database, so arbitrary SQLite files are not merged.
constexpr int kApplicationId = 0x444D4D31;
constexpr int kSchemaVersion = 1;
// Imports commit stored files in batches of about this size, so thumbnails can be saved in between.
constexpr size_t kBatchBytes = 64u << 20;

std::string Hex(const uint8_t* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
std::optional<Hash> ParseHex(std::string_view text) {
    if (text.size() != 64) return std::nullopt;
    Hash result{};
    for (size_t i = 0; i < 64; ++i) {
        const char c = text[i];
        const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (value < 0) return std::nullopt;
        result[i / 2] = static_cast<uint8_t>(result[i / 2] << 4 | value);
    }
    return result;
}
std::string RandomId() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw IoError("Could not generate a temporary file name.");
    return Hex(bytes.data(), bytes.size());
}
Hash Sha256(const uint8_t* data, size_t size) {
    Hash result{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(size), result.data(), static_cast<ULONG>(result.size())) < 0)
        throw IoError("Could not compute a SHA-256 checksum.");
    return result;
}
std::string Key(const fs::path& path) {
    std::wstring name = fs::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    return Narrow(name);
}
bool SamePath(const fs::path& a, const fs::path& b) { return Key(a) == Key(b); }
int64_t Stamp(const fs::path& path) { return fs::last_write_time(path).time_since_epoch().count(); }

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
    bool Null(int column) const { return sqlite3_column_type(statement_, column) == SQLITE_NULL; }
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

class LibraryDb {
public:
    explicit LibraryDb(const fs::path& path) {
        fs::create_directories(path.parent_path());
        const auto name = Narrow(path.wstring());
        if (sqlite3_open_v2(name.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
            const std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);
            throw IoError("Could not open the library database " + name + ": " + message);
        }
        try {
            sqlite3_busy_timeout(db_, 60000);
            // WAL lets thumbnails and the gallery read while an import writes. Closing the last connection folds the
            // log back into library.db, so a closed library is still a single file.
            Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
            Migrate();
        } catch (...) {
            sqlite3_close(db_);
            throw;
        }
    }
    ~LibraryDb() {
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        sqlite3_close(db_);
    }
    LibraryDb(const LibraryDb&) = delete;
    LibraryDb& operator=(const LibraryDb&) = delete;
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
    // Long statements stop with OperationCancelled once the token is cancelled.
    void Cancel(CancellationToken token) {
        token_ = std::move(token);
        sqlite3_progress_handler(db_, 10000, [](void* self) { return static_cast<LibraryDb*>(self)->token_.IsCancelled() ? 1 : 0; }, this);
    }
    std::optional<std::string> Setting(const std::string& key) {
        Statement select(db_, "SELECT value FROM settings WHERE key = ?");
        select.Bind(1, key);
        if (!select.Step()) return std::nullopt;
        return select.Text(0);
    }
    void SetSetting(const std::string& key, const std::string& value) {
        Statement(db_, "INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value").Bind(1, key).Bind(2, value).Run();
    }

private:
    int64_t Pragma(const char* sql) {
        Statement statement(db_, sql);
        return statement.Step() ? statement.Int(0) : 0;
    }
    void Migrate() {
        const auto current = Pragma("PRAGMA user_version");
        if (current > kSchemaVersion) throw InvalidData("The library database was written by a newer version of the mod manager.");
        if (current == kSchemaVersion) {
            if (Pragma("PRAGMA application_id") != kApplicationId) throw InvalidData("This file is not a Deadlock mod library.");
            return;
        }
        // Another connection may be creating the schema at the same moment; decide under the write lock.
        Exec("BEGIN IMMEDIATE");
        try {
            if (Pragma("PRAGMA user_version") != 0) { Exec("COMMIT"); Migrate(); return; }
            if (Pragma("SELECT count(*) FROM sqlite_schema") != 0) throw InvalidData("This file is not a Deadlock mod library.");
        } catch (...) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
        Exec(R"sql(
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
-- Where each mod was imported from. A repeat import of an unchanged original skips reading it again.
CREATE TABLE archive_origins(
    archive INTEGER NOT NULL REFERENCES archives(id),
    path TEXT NOT NULL,
    size INTEGER NOT NULL,
    stamp INTEGER NOT NULL,
    chunks TEXT NOT NULL,
    PRIMARY KEY(path, size, stamp, chunks)
) WITHOUT ROWID;
-- Copies from the folder-based store of earlier versions, by lowercase path. Not merged into other libraries.
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
COMMIT;
)sql");
    }
    sqlite3* db_ = nullptr;
    CancellationToken token_;
};
static_assert(kApplicationId == 1145916721, "Update the schema script's application_id.");

// Rolls back unless committed.
class Transaction {
public:
    explicit Transaction(LibraryDb& db) : db_(db) { db_.Exec("BEGIN IMMEDIATE"); }
    ~Transaction() { if (open_) sqlite3_exec(db_.Get(), "ROLLBACK", nullptr, nullptr, nullptr); }
    void Commit() { db_.Exec("COMMIT"); open_ = false; }
private:
    LibraryDb& db_;
    bool open_ = true;
};

struct StoredFile { std::string Path; Hash Blob; };

Hash ContentHash(std::vector<StoredFile> files) {
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.Path < b.Path; });
    std::vector<uint8_t> manifest;
    for (const auto& file : files) {
        manifest.insert(manifest.end(), file.Path.begin(), file.Path.end());
        manifest.push_back(0);
        manifest.insert(manifest.end(), file.Blob.begin(), file.Blob.end());
    }
    return Sha256(manifest.data(), manifest.size());
}
std::vector<StoredFile> ArchiveFiles(LibraryDb& db, int64_t archive) {
    Statement select(db.Get(), "SELECT path, blob FROM archive_entries WHERE archive = ?");
    select.Bind(1, archive);
    std::vector<StoredFile> files;
    while (select.Step()) files.push_back({select.Text(0), select.HashColumn(1)});
    return files;
}

// Adds files to the blob table, committing in batches. A cancelled import can leave stored files that no mod uses
// yet; a later import of the same content reuses them.
class BlobWriter {
public:
    explicit BlobWriter(LibraryDb& db) : db_(db), exists_(db.Get(), "SELECT 1 FROM blobs WHERE hash = ?"),
        insert_(db.Get(), "INSERT OR IGNORE INTO blobs(hash, size, crc, compression, data) VALUES(?, ?, ?, ?, ?)") {}
    ~BlobWriter() { if (open_) sqlite3_exec(db_.Get(), "ROLLBACK", nullptr, nullptr, nullptr); }
    Hash Add(const std::vector<uint8_t>& bytes) {
        const auto hash = Sha256(bytes.data(), bytes.size());
        exists_.Bind(1, hash);
        const bool stored = exists_.Step();
        exists_.Reset();
        if (stored) return hash;
        std::vector<uint8_t> packed(ZSTD_compressBound(bytes.size()));
        const size_t size = ZSTD_compress(packed.data(), packed.size(), bytes.data(), bytes.size(), 3);
        // Many resources are already compressed, so keep them raw when zstd does not help.
        const bool compressed = !ZSTD_isError(size) && size < bytes.size();
        if (compressed) packed.resize(size);
        if (!open_) { db_.Exec("BEGIN IMMEDIATE"); open_ = true; }
        insert_.Bind(1, hash).Bind(2, static_cast<int64_t>(bytes.size())).Bind(3, static_cast<int64_t>(VpkArchive::Crc(bytes)))
            .Bind(4, compressed ? 1 : 0).Bind(5, compressed ? packed : bytes).Run();
        pending_ += compressed ? packed.size() : bytes.size();
        if (pending_ >= kBatchBytes) Flush();
        return hash;
    }
    void Flush() {
        if (open_) { db_.Exec("COMMIT"); open_ = false; }
        pending_ = 0;
    }
private:
    LibraryDb& db_;
    Statement exists_;
    Statement insert_;
    size_t pending_ = 0;
    bool open_ = false;
};

struct Origin { std::string Path; uint64_t Size = 0; int64_t Stamp = 0; std::string Chunks; };

// The original location and file versions of an archive and its chunks, for recognizing unchanged repeat imports.
Origin OriginOf(const fs::path& full, const VpkArchive& archive) {
    auto chunks = nlohmann::json::array();
    std::set<uint16_t> chunk_ids;
    for (const auto& entry : archive.Entries()) if (entry.ArchiveIndex != VpkArchive::EmbeddedArchive) chunk_ids.insert(entry.ArchiveIndex);
    auto basename = full.stem().wstring();
    if (basename.ends_with(L"_dir")) basename.resize(basename.size() - 4);
    for (const auto id : chunk_ids) {
        std::wostringstream name;
        name << basename << L"_" << std::setw(3) << std::setfill(L'0') << id << L".vpk";
        const auto chunk = full.parent_path() / name.str();
        chunks.push_back({{"index", id}, {"size", fs::file_size(chunk)}, {"stamp", Stamp(chunk)}});
    }
    return {Key(full), fs::file_size(full), Stamp(full), chunks.dump()};
}
std::optional<int64_t> FindOrigin(LibraryDb& db, const Origin& origin) {
    Statement select(db.Get(), "SELECT archive FROM archive_origins WHERE path = ? AND size = ? AND stamp = ? AND chunks = ?");
    select.Bind(1, origin.Path).Bind(2, static_cast<int64_t>(origin.Size)).Bind(3, origin.Stamp).Bind(4, origin.Chunks);
    if (!select.Step()) return std::nullopt;
    return select.Int(0);
}
void AddOrigin(LibraryDb& db, int64_t archive, const Origin& origin) {
    Statement(db.Get(), "INSERT OR IGNORE INTO archive_origins(archive, path, size, stamp, chunks) VALUES(?, ?, ?, ?, ?)")
        .Bind(1, archive).Bind(2, origin.Path).Bind(3, static_cast<int64_t>(origin.Size)).Bind(4, origin.Stamp).Bind(5, origin.Chunks).Run();
}

// Stores every file of the archive and returns its archive row, reusing one with identical content.
int64_t StoreArchive(LibraryDb& db, const fs::path& full, const std::optional<Origin>& origin, CancellationToken cancellation) {
    const auto archive = VpkArchive::Open(full);
    auto lease = archive.LockSources();
    if (origin) if (const auto known = FindOrigin(db, *origin)) return *known;
    std::vector<StoredFile> files;
    {
        BlobWriter blobs(db);
        for (const auto& entry : archive.Entries()) {
            cancellation.ThrowIfCancelled();
            // Read checks each file's CRC, so a damaged archive is never stored.
            files.push_back({VpkArchive::Normalize(entry.FullPath), blobs.Add(archive.Read(entry, cancellation))});
        }
        blobs.Flush();
    }
    cancellation.ThrowIfCancelled();
    const auto content = ContentHash(files);
    Transaction transaction(db);
    Statement find(db.Get(), "SELECT id FROM archives WHERE content_hash = ?");
    find.Bind(1, content);
    int64_t id = 0;
    if (find.Step()) {
        id = find.Int(0);
    } else {
        Statement(db.Get(), "INSERT INTO archives(content_hash, name, imported_at) VALUES(?, ?, ?)")
            .Bind(1, content).Bind(2, Narrow(full.filename().wstring()))
            .Bind(3, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()).Run();
        id = sqlite3_last_insert_rowid(db.Get());
        Statement insert(db.Get(), "INSERT INTO archive_entries(archive, path, blob) VALUES(?, ?, ?)");
        for (const auto& file : files) insert.Bind(1, id).Bind(2, file.Path).Bind(3, file.Blob).Run();
    }
    find.Reset();
    if (origin) AddOrigin(db, id, *origin);
    transaction.Commit();
    return id;
}

struct ArchiveRow { int64_t Id = 0; Hash Content{}; std::string Name; };
std::optional<ArchiveRow> FindArchive(LibraryDb& db, const Hash& content) {
    Statement select(db.Get(), "SELECT id, name FROM archives WHERE content_hash = ?");
    select.Bind(1, content);
    if (!select.Step()) return std::nullopt;
    return ArchiveRow{select.Int(0), content, select.Text(1)};
}
ArchiveRow GetArchive(LibraryDb& db, int64_t id) {
    Statement select(db.Get(), "SELECT content_hash, name FROM archives WHERE id = ?");
    select.Bind(1, id);
    if (!select.Step()) throw InvalidData("The library database is missing a stored mod.");
    return {id, select.HashColumn(0), select.Text(1)};
}
// Stored names come from imported files or shared databases, so only a plain .vpk file name is used.
std::wstring SafeName(const std::string& name) {
    const auto wide = fs::path(Widen(name)).filename().wstring();
    const bool valid = !wide.empty() && wide.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos
        && EqualsIgnoreCase(Narrow(fs::path(wide).extension().wstring()), ".vpk");
    return valid ? wide : L"mod.vpk";
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

namespace {
struct Context {
    const fs::path& Root;
    LibraryDb& Db;
    fs::path Cache() const { return Root / L"cache"; }
    fs::path CachePath(const ArchiveRow& row) const { return Cache() / Widen(Hex(row.Content.data(), row.Content.size())) / SafeName(row.Name); }
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
    std::optional<Hash> CacheContent(const fs::path& path) const {
        const auto full = fs::absolute(path).lexically_normal();
        if (!SamePath(full.parent_path().parent_path(), Cache())) return std::nullopt;
        return ParseHex(Narrow(full.parent_path().filename().wstring()));
    }
    std::optional<int64_t> ArchiveFor(const fs::path& path) const {
        const auto content = CacheContent(path);
        if (!content) return std::nullopt;
        const auto row = FindArchive(Db, *content);
        return row ? std::optional(row->Id) : std::nullopt;
    }
    // Writes the archive from the database, unless an unchanged copy is already there.
    fs::path Materialize(int64_t id, CancellationToken cancellation) const {
        const auto row = GetArchive(Db, id);
        const auto destination = CachePath(row);
        if (Fresh(destination)) return destination;
        fs::create_directories(destination.parent_path());
        const auto temp = destination.parent_path() / Widen("build-" + RandomId() + ".tmp");
        try {
            Statement entries(Db.Get(), "SELECT e.path, b.hash, b.size, b.crc FROM archive_entries e JOIN blobs b ON b.hash = e.blob WHERE e.archive = ? ORDER BY e.path");
            entries.Bind(1, id);
            auto read = std::make_shared<Statement>(Db.Get(), "SELECT compression, data, size FROM blobs WHERE hash = ?");
            std::vector<VpkNewFile> files;
            while (entries.Step()) {
                const auto size = entries.Int(2);
                if (size < 0 || size > std::numeric_limits<int32_t>::max()) throw InvalidData("The library database contains an invalid file size.");
                files.push_back({entries.Text(0), static_cast<uint32_t>(entries.Int(3)), static_cast<uint32_t>(size),
                    [read, hash = entries.HashColumn(1), path = entries.Text(0)](const std::function<void(const uint8_t*, size_t)>& sink) {
                        read->Bind(1, hash);
                        if (!read->Step()) throw InvalidData("The library database is missing " + path + ".");
                        auto data = read->Blob(1);
                        const auto compression = read->Int(0);
                        const auto size = static_cast<size_t>(read->Int(2));
                        read->Reset();
                        if (compression == 1) {
                            std::vector<uint8_t> raw(size);
                            const size_t written = ZSTD_decompress(raw.data(), raw.size(), data.data(), data.size());
                            if (ZSTD_isError(written) || written != size) throw InvalidData("Stored file " + path + " is damaged.");
                            data = std::move(raw);
                        } else if (compression != 0) {
                            throw InvalidData("Stored file " + path + " uses an unknown compression.");
                        }
                        if (data.size() != size || Sha256(data.data(), data.size()) != hash) throw InvalidData("Stored file " + path + " is damaged.");
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
    int64_t SourceArchive(const fs::path& source, CancellationToken cancellation) const {
        const auto full = fs::absolute(source).lexically_normal();
        if (!EqualsIgnoreCase(Narrow(full.extension().wstring()), ".vpk")) throw InvalidData("Import requires a .vpk archive.");
        if (const auto id = ArchiveFor(full); id && Fresh(full)) return *id;
        // A rebuilt copy changed outside the store, for example by a repair, is imported as a new version.
        std::optional<Origin> origin;
        if (!CacheContent(full)) origin = OriginOf(full, VpkArchive::Open(full));
        return StoreArchive(Db, full, origin, cancellation);
    }
    std::vector<int64_t> Sources() const {
        Statement select(Db.Get(), "SELECT archive FROM library_sources ORDER BY position");
        std::vector<int64_t> result;
        while (select.Step()) result.push_back(select.Int(0));
        return result;
    }
    std::vector<std::pair<int64_t, std::string>> Selected() const {
        Statement select(Db.Get(), "SELECT archive, model FROM library_selected ORDER BY position");
        std::vector<std::pair<int64_t, std::string>> result;
        while (select.Step()) result.emplace_back(select.Int(0), select.Text(1));
        return result;
    }
    // Appends sources and selections that are not in the collection yet. Existing entries keep their order.
    void AddToCollection(const std::vector<int64_t>& sources, const std::vector<std::pair<int64_t, std::string>>& selected) const {
        Statement source(Db.Get(), "INSERT OR IGNORE INTO library_sources(position, archive) VALUES((SELECT coalesce(max(position), 0) + 1 FROM library_sources), ?)");
        for (const auto id : sources) source.Bind(1, id).Run();
        Statement selection(Db.Get(), "INSERT OR IGNORE INTO library_selected(position, archive, model) VALUES((SELECT coalesce(max(position), 0) + 1 FROM library_selected), ?, ?)");
        for (const auto& [id, model] : selected) selection.Bind(1, id).Bind(2, model).Run();
    }
    void SetGameIfUnset(const fs::path& game) const {
        if (game.empty() || Db.Setting("game_archive")) return;
        Db.SetSetting("game_archive", Narrow(fs::absolute(game).lexically_normal().wstring()));
    }

    // Merges the folder-based store of earlier versions: every stored VPK, its import origin, the collection and
    // the selections. The old files are only read, never moved or removed.
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

        std::map<std::string, int64_t> imported;
        Statement known(Db.Get(), "SELECT archive FROM legacy_paths WHERE path = ? AND size = ? AND stamp = ?");
        for (size_t i = 0; i < archives.size(); ++i) {
            cancellation.ThrowIfCancelled();
            if (progress) progress(i, archives.size());
            const auto& path = archives[i];
            try {
                const auto size = static_cast<int64_t>(fs::file_size(path));
                const auto stamp = Stamp(path);
                known.Bind(1, Key(path)).Bind(2, size).Bind(3, stamp);
                std::optional<int64_t> id;
                if (known.Step()) id = known.Int(0);
                known.Reset();
                if (!id) {
                    std::optional<Origin> origin;
                    try {
                        std::ifstream input(path.parent_path() / L"origin.json", std::ios::binary);
                        nlohmann::json json; input >> json;
                        const fs::path original = Widen(json.at("original").get<std::string>());
                        if (EqualsIgnoreCase(Narrow(original.filename().wstring()), Narrow(path.filename().wstring())))
                            origin = Origin{Key(original), json.at("size").get<uint64_t>(), json.at("stamp").get<int64_t>(), json.value("chunks", nlohmann::json::array()).dump()};
                    } catch (const std::exception&) { /* An import without a readable record still has its archive. */ }
                    id = StoreArchive(Db, path, std::nullopt, cancellation);
                    Transaction transaction(Db);
                    if (origin) AddOrigin(Db, *id, *origin);
                    Statement(Db.Get(), "INSERT OR REPLACE INTO legacy_paths(path, archive, size, stamp) VALUES(?, ?, ?, ?)")
                        .Bind(1, Key(path)).Bind(2, *id).Bind(3, size).Bind(4, stamp).Run();
                    transaction.Commit();
                }
                imported[Key(path)] = *id;
            } catch (const OperationCancelled&) { throw; }
            catch (const std::exception& failure) { notice += Narrow(path.wstring()) + ": " + failure.what() + "\n"; }
        }
        if (progress) progress(archives.size(), archives.size());

        std::optional<ModelLibrary> library;
        for (const auto& manifest : {main, backup}) {
            if (library || !fs::exists(manifest)) continue;
            try { library = ModelLibrary::Load(manifest); } catch (const std::exception&) {}
        }
        if (!library && (fs::exists(main) || fs::exists(backup)) && !Db.Setting("legacy_collection_merged"))
            notice += "The earlier collection list could not be read, so its mods were added without restoring the collection. It is still in " + Narrow(Root.wstring()) + ".\n";
        // The collection is merged once, so sources removed afterwards do not come back on the next start.
        if (library && !Db.Setting("legacy_collection_merged")) {
            std::map<std::string, int64_t> sources;
            for (const auto& source : library->Sources) {
                cancellation.ThrowIfCancelled();
                auto found = imported.find(Key(source));
                if (found != imported.end()) { sources[Key(source)] = found->second; continue; }
                // Sources outside the old store were used in place; store them now if they still exist.
                try { sources[Key(source)] = SourceArchive(source, cancellation); }
                catch (const OperationCancelled&) { throw; }
                catch (const std::exception& failure) { notice += Narrow(source.wstring()) + ": " + failure.what() + "\n"; }
            }
            std::vector<int64_t> order;
            for (const auto& source : library->Sources) if (const auto found = sources.find(Key(source)); found != sources.end()) order.push_back(found->second);
            std::vector<std::pair<int64_t, std::string>> selected;
            for (const auto& model : library->Selected)
                if (const auto found = sources.find(Key(model.Archive)); found != sources.end()) selected.emplace_back(found->second, model.Model);
            Transaction transaction(Db);
            AddToCollection(order, selected);
            SetGameIfUnset(library->GameArchive);
            Db.SetSetting("legacy_collection_merged", "1");
            transaction.Commit();
        }
        if (notice.empty()) Db.SetSetting("legacy_store_merged", "1");
    }
};
}

fs::path LocalModStore::Import(const fs::path& source, CancellationToken cancellation) const {
    cancellation.ThrowIfCancelled();
    LibraryDb db(Database());
    db.Cancel(cancellation);
    const Context context{root_, db};
    return context.Materialize(context.SourceArchive(source, cancellation), cancellation);
}

ModelLibrary LocalModStore::Load(CancellationToken cancellation, const StoreProgress& progress, std::string* notice) const {
    LibraryDb db(Database());
    db.Cancel(cancellation);
    const Context context{root_, db};
    std::string problems;
    context.MergeLegacy(cancellation, progress, problems);
    ModelLibrary result;
    std::map<int64_t, fs::path> paths;
    const auto sources = context.Sources();
    for (size_t i = 0; i < sources.size(); ++i) {
        cancellation.ThrowIfCancelled();
        if (progress) progress(i, sources.size());
        try { paths[sources[i]] = context.Materialize(sources[i], cancellation); }
        catch (const OperationCancelled&) { throw; }
        catch (const std::exception& error) {
            // Keep the source in the collection; the gallery reports that it cannot be opened.
            const auto row = GetArchive(db, sources[i]);
            paths[sources[i]] = context.CachePath(row);
            problems += row.Name + ": " + error.what() + "\n";
        }
        result.Sources.push_back(paths[sources[i]]);
    }
    for (const auto& [id, model] : context.Selected())
        if (const auto found = paths.find(id); found != paths.end()) result.Selected.push_back({found->second, model});
    if (const auto game = db.Setting("game_archive")) {
        const fs::path path = Widen(*game);
        // A library shared from another computer keeps its game path, which only applies where it exists.
        std::error_code error;
        if (fs::is_regular_file(path, error)) result.GameArchive = path;
    }
    if (notice) *notice = problems;
    return result;
}

void LocalModStore::Save(const ModelLibrary& library) const {
    LibraryDb db(Database());
    const Context context{root_, db};
    Transaction transaction(db);
    std::vector<int64_t> sources;
    std::map<std::string, int64_t> ids;
    for (const auto& source : library.Sources) {
        const auto id = context.ArchiveFor(source);
        if (!id) throw InvalidData(Narrow(source.wstring()) + " is not stored in the library database.");
        ids[Key(source)] = *id;
        if (std::find(sources.begin(), sources.end(), *id) == sources.end()) sources.push_back(*id);
    }
    db.Exec("DELETE FROM library_sources; DELETE FROM library_selected;");
    std::vector<std::pair<int64_t, std::string>> selected;
    for (const auto& model : library.Selected) {
        const auto found = ids.find(Key(model.Archive));
        if (found == ids.end()) throw InvalidData("Selected model is not in the library sources.");
        selected.emplace_back(found->second, VpkArchive::Normalize(model.Model));
    }
    context.AddToCollection(sources, selected);
    if (!library.GameArchive.empty()) db.SetSetting("game_archive", Narrow(fs::absolute(library.GameArchive).lexically_normal().wstring()));
    transaction.Commit();
}

void LocalModStore::Merge(const fs::path& other, CancellationToken cancellation) const {
    if (SamePath(other, Database())) throw InvalidData("Choose a library other than the one already open.");
    if (!fs::is_regular_file(other)) throw IoError("Could not find " + Narrow(other.wstring()) + ".");
    {
        // Checks the file's identity and version without changing it.
        sqlite3* check = nullptr;
        const auto name = Narrow(other.wstring());
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
        if (application != kApplicationId || version < 1) throw InvalidData(Narrow(other.filename().wstring()) + " is not a Deadlock mod library.");
        if (version > kSchemaVersion) throw InvalidData(Narrow(other.filename().wstring()) + " was written by a newer version of the mod manager.");
    }
    LibraryDb db(Database());
    db.Cancel(cancellation);
    const Context context{root_, db};
    // Read-only, so merging never changes the shared file.
    std::string uri = "file:";
    for (const char c : Narrow(fs::absolute(other).wstring())) {
        if (c == '\\') uri += '/';
        else if (c == '%' || c == '?' || c == '#') { char escaped[4]; snprintf(escaped, sizeof escaped, "%%%02X", static_cast<unsigned char>(c)); uri += escaped; }
        else uri += c;
    }
    uri += "?mode=ro";
    Statement(db.Get(), "ATTACH DATABASE ? AS other").Bind(1, uri).Run();
    try {
        Transaction transaction(db);
        Statement before(db.Get(), "SELECT coalesce(max(id), 0) FROM archives");
        before.Step();
        const auto last = before.Int(0);
        const std::string copy = R"sql(
INSERT OR IGNORE INTO blobs(hash, size, crc, compression, data) SELECT hash, size, crc, compression, data FROM other.blobs;
INSERT OR IGNORE INTO archives(content_hash, name, imported_at) SELECT content_hash, name, imported_at FROM other.archives;
INSERT OR IGNORE INTO archive_entries(archive, path, blob)
    SELECT a.id, e.path, e.blob FROM other.archive_entries e
    JOIN other.archives o ON o.id = e.archive JOIN archives a ON a.content_hash = o.content_hash
    WHERE a.id > )sql" + std::to_string(last) + R"sql(;
INSERT OR IGNORE INTO archive_origins(archive, path, size, stamp, chunks)
    SELECT a.id, r.path, r.size, r.stamp, r.chunks FROM other.archive_origins r
    JOIN other.archives o ON o.id = r.archive JOIN archives a ON a.content_hash = o.content_hash;
INSERT OR IGNORE INTO thumbnails(key, png) SELECT key, png FROM other.thumbnails;
)sql";
        db.Exec(copy.c_str());
        // Content identifiers come from another file, so each new mod must match the files it lists.
        Statement added(db.Get(), "SELECT id, content_hash FROM archives WHERE id > ?");
        added.Bind(1, last);
        while (added.Step()) {
            cancellation.ThrowIfCancelled();
            if (ContentHash(ArchiveFiles(db, added.Int(0))) != added.HashColumn(1))
                throw InvalidData(Narrow(other.filename().wstring()) + " is damaged: a stored mod does not match its checksum.");
        }
        std::vector<int64_t> sources;
        Statement other_sources(db.Get(), "SELECT a.id FROM other.library_sources s JOIN other.archives o ON o.id = s.archive JOIN archives a ON a.content_hash = o.content_hash ORDER BY s.position");
        while (other_sources.Step()) sources.push_back(other_sources.Int(0));
        std::vector<std::pair<int64_t, std::string>> selected;
        Statement other_selected(db.Get(), "SELECT a.id, s.model FROM other.library_selected s JOIN other.archives o ON o.id = s.archive JOIN archives a ON a.content_hash = o.content_hash ORDER BY s.position");
        while (other_selected.Step()) selected.emplace_back(other_selected.Int(0), other_selected.Text(1));
        context.AddToCollection(sources, selected);
        Statement other_game(db.Get(), "SELECT value FROM other.settings WHERE key = 'game_archive'");
        if (other_game.Step()) {
            const fs::path game = Widen(other_game.Text(0));
            std::error_code error;
            if (fs::is_regular_file(game, error)) context.SetGameIfUnset(game);
        }
        transaction.Commit();
    } catch (...) {
        sqlite3_exec(db.Get(), "DETACH DATABASE other", nullptr, nullptr, nullptr);
        throw;
    }
    db.Exec("DETACH DATABASE other");
}

void LocalModStore::Export(const fs::path& destination) const {
    if (SamePath(destination, Database())) throw InvalidData("Choose a location other than the library itself.");
    const auto full = fs::absolute(destination).lexically_normal();
    const auto temp = full.parent_path() / Widen("library-" + RandomId() + ".tmp");
    try {
        {
            LibraryDb db(Database());
            // A compact copy of the committed library, written while the store stays usable.
            Statement(db.Get(), "VACUUM INTO ?").Bind(1, Narrow(temp.wstring())).Run();
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
    if (!ParseHex(id)) return std::nullopt;
    return id;
}

std::vector<fs::path> LocalModStore::LegacyPaths(const fs::path& archive) const {
    const auto id = ContentId(archive);
    if (!id) return {};
    LibraryDb db(Database());
    Statement select(db.Get(), "SELECT l.path FROM legacy_paths l JOIN archives a ON a.id = l.archive WHERE a.content_hash = ?");
    select.Bind(1, *ParseHex(*id));
    std::vector<fs::path> result;
    while (select.Step()) result.emplace_back(Widen(select.Text(0)));
    return result;
}

std::optional<std::vector<uint8_t>> LocalModStore::Thumbnail(const std::string& key) const {
    LibraryDb db(Database());
    Statement select(db.Get(), "SELECT png FROM thumbnails WHERE key = ?");
    select.Bind(1, key);
    if (!select.Step()) return std::nullopt;
    return select.Blob(0);
}

void LocalModStore::SaveThumbnail(const std::string& key, const std::vector<uint8_t>& png) const {
    LibraryDb db(Database());
    Statement(db.Get(), "INSERT OR REPLACE INTO thumbnails(key, png) VALUES(?, ?)").Bind(1, key).Bind(2, png).Run();
}
}
