#include "content_store.hpp"

#include "archive_transaction.hpp"
#include "content_hash.hpp"
#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"
#include "strings.hpp"
#include "vpk.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace skeleton_repair {
namespace {

constexpr const char* kBusyMessage = "The model library is in use by another process.";
constexpr const char* kSchemaMessage = "Unsupported model library schema.";
constexpr const char* kChunkMessage = "Import the directory VPK, not a chunk file.";
constexpr const char* kGameArchiveMessage = "That file is the game archive. Choose a mod VPK from the addons folder.";
constexpr const char* kGameFolderMessage = "Choose a mod outside the game's archive folder, such as a VPK in addons.";
constexpr const char* kMissingMessage = "Could not find the mod VPK.";
constexpr const char* kMissingObservation = "The model library has no observation with that id.";

constexpr const char* kCreateSchema = R"sql(
CREATE TABLE meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE blobs (
  sha256 BLOB PRIMARY KEY,
  size INTEGER NOT NULL,
  codec TEXT NOT NULL,
  stored_size INTEGER NOT NULL,
  data_sha256 BLOB,
  bytes_state TEXT NOT NULL,
  created_utc TEXT NOT NULL
);
CREATE TABLE lineages (
  id TEXT PRIMARY KEY,
  root_sha256 BLOB NOT NULL,
  created_utc TEXT NOT NULL
);
CREATE TABLE snapshots (
  id TEXT PRIMARY KEY,
  manifest_sha256 BLOB NOT NULL UNIQUE,
  created_utc TEXT NOT NULL,
  tool_version TEXT
);
CREATE TABLE memberships (
  snapshot_id TEXT NOT NULL,
  path TEXT NOT NULL,
  path_key TEXT NOT NULL,
  blob_sha256 BLOB NOT NULL,
  crc32 INTEGER NOT NULL,
  kind TEXT NOT NULL,
  lineage_id TEXT NOT NULL,
  PRIMARY KEY (snapshot_id, path_key)
);
CREATE TABLE observations (
  id TEXT PRIMARY KEY,
  snapshot_id TEXT NOT NULL,
  absolute_path TEXT,
  container_sha256 BLOB,
  file_size INTEGER,
  observed_utc TEXT NOT NULL,
  source TEXT NOT NULL
);
CREATE TABLE transforms (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  tool_version TEXT NOT NULL,
  finished_utc TEXT NOT NULL,
  summary_json TEXT NOT NULL,
  input_snapshot_id TEXT NOT NULL,
  output_snapshot_id TEXT NOT NULL
);
CREATE TABLE edges (
  id INTEGER PRIMARY KEY,
  transform_id TEXT NOT NULL,
  relation TEXT NOT NULL,
  input_blob BLOB,
  input_path_key TEXT,
  input_data_sha256 BLOB,
  output_blob BLOB,
  output_path_key TEXT,
  output_data_sha256 BLOB,
  lineage_id TEXT,
  detail_json TEXT
);
CREATE TABLE refs (
  snapshot_id TEXT NOT NULL,
  from_path_key TEXT NOT NULL,
  name TEXT NOT NULL,
  resolved_path_key TEXT,
  resolved_blob BLOB,
  PRIMARY KEY (snapshot_id, from_path_key, name)
);
CREATE INDEX idx_memberships_blob ON memberships(blob_sha256);
CREATE INDEX idx_memberships_lineage ON memberships(lineage_id);
CREATE INDEX idx_edges_output ON edges(output_blob);
CREATE INDEX idx_edges_input ON edges(input_blob);
CREATE INDEX idx_edges_lineage ON edges(lineage_id);
CREATE INDEX idx_blobs_data ON blobs(data_sha256);
)sql";

bool IsBusy(int rc) {
    const int primary = rc & 0xFF;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

void Check(sqlite3* db, int rc) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return;
    }
    if (IsBusy(rc)) {
        throw IoError(kBusyMessage);
    }
    const char* message = db != nullptr ? sqlite3_errmsg(db) : nullptr;
    throw IoError(message != nullptr && message[0] != '\0' ? message : "Could not update the model library.");
}

void Exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc == SQLITE_OK) {
        return;
    }
    std::string message = error != nullptr ? error : "";
    sqlite3_free(error);
    if (IsBusy(rc)) {
        throw IoError(kBusyMessage);
    }
    throw IoError(message.empty() ? "Could not update the model library." : message);
}

void Rollback(sqlite3* db) {
    if (db != nullptr && sqlite3_get_autocommit(db) == 0) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        Check(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr));
    }

    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void BindBlob(int index, std::span<const uint8_t> bytes) {
        const void* data = bytes.empty() ? static_cast<const void*>("") : static_cast<const void*>(bytes.data());
        Check(db_, sqlite3_bind_blob(stmt_, index, data, static_cast<int>(bytes.size()), SQLITE_TRANSIENT));
    }

    void BindText(int index, std::string_view text) {
        if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw InvalidData("A library value is too large.");
        }
        const char* data = text.empty() ? "" : text.data();
        Check(db_, sqlite3_bind_text(stmt_, index, data, static_cast<int>(text.size()), SQLITE_TRANSIENT));
    }

    void BindInt64(int index, int64_t value) { Check(db_, sqlite3_bind_int64(stmt_, index, value)); }

    void BindNull(int index) { Check(db_, sqlite3_bind_null(stmt_, index)); }

    bool Step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        Check(db_, rc);
        return false;
    }

    void Execute() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) {
            Check(db_, rc == SQLITE_ROW ? SQLITE_ERROR : rc);
        }
    }

    std::string ColumnText(int index) const {
        const auto* text = sqlite3_column_text(stmt_, index);
        if (text == nullptr) {
            return {};
        }
        return reinterpret_cast<const char*>(text);
    }

    int64_t ColumnInt64(int index) const { return sqlite3_column_int64(stmt_, index); }

    bool ColumnNull(int index) const { return sqlite3_column_type(stmt_, index) == SQLITE_NULL; }

    BlobId ColumnBlobId(int index) const {
        const void* data = sqlite3_column_blob(stmt_, index);
        const int size = sqlite3_column_bytes(stmt_, index);
        if (data == nullptr || size != 32) {
            throw InvalidData("The model library has a corrupt blob id.");
        }
        BlobId id;
        std::memcpy(id.Bytes.data(), data, 32);
        return id;
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

std::string AsciiLower(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return out;
}

std::string PathText(const std::filesystem::path& path) { return Narrow(path.wstring()); }

std::string UtcNow() {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    const int ms = static_cast<int>(millis % 1000);
    std::tm time{};
    gmtime_s(&time, &seconds);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", time.tm_year + 1900, time.tm_mon + 1,
        time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec, ms);
    return buffer;
}

void RandomBytes(uint8_t* data, ULONG size) {
    if (BCryptGenRandom(nullptr, data, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw IoError("Could not generate an id.");
    }
}

std::string NewId() {
    std::array<uint8_t, 16> bytes{};
    RandomBytes(bytes.data(), static_cast<ULONG>(bytes.size()));
    return ToLowerHex(bytes);
}

std::string NewUuid() {
    std::array<uint8_t, 16> bytes{};
    RandomBytes(bytes.data(), static_cast<ULONG>(bytes.size()));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
    const std::string hex = ToLowerHex(bytes);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-"
        + hex.substr(20);
}

bool TableExists(sqlite3* db, const char* name) {
    Statement stmt(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
    stmt.BindText(1, name);
    return stmt.Step();
}

bool SchemaIsVersion1(sqlite3* db) {
    Statement stmt(db, "SELECT value FROM meta WHERE key = 'schema_version'");
    if (!stmt.Step() || stmt.ColumnNull(0)) {
        return false;
    }
    return stmt.ColumnText(0) == "1";
}

void InitializeDatabase(sqlite3* db) {
    Exec(db, "PRAGMA journal_mode = WAL;");
    Exec(db, "PRAGMA foreign_keys = ON;");
    sqlite3_busy_timeout(db, 1000);
    // An existing library is only read here, so opening it does not wait for another connection's write.
    if (TableExists(db, "meta")) {
        if (!SchemaIsVersion1(db)) {
            throw InvalidData(kSchemaMessage);
        }
        return;
    }
    Exec(db, "BEGIN IMMEDIATE;");
    try {
        if (!TableExists(db, "meta")) {
            Exec(db, kCreateSchema);
            Exec(db, "INSERT INTO meta (key, value) VALUES ('schema_version', '1');");
        } else if (!SchemaIsVersion1(db)) {
            throw InvalidData(kSchemaMessage);
        }
        Exec(db, "COMMIT;");
    } catch (...) {
        Rollback(db);
        throw;
    }
}

bool IsChunkName(std::string_view filename) {
    const std::string lower = AsciiLower(filename);
    if (lower.size() < 8 || !lower.ends_with(".vpk")) {
        return false;
    }
    const size_t underscore = lower.size() - 8;
    if (lower[underscore] != '_') {
        return false;
    }
    return std::isdigit(static_cast<unsigned char>(lower[underscore + 1])) != 0
        && std::isdigit(static_cast<unsigned char>(lower[underscore + 2])) != 0
        && std::isdigit(static_cast<unsigned char>(lower[underscore + 3])) != 0;
}

void RejectGameArchive(const std::filesystem::path& absolute, bool reject_game_folder) {
    if (EqualsIgnoreCase(PathText(absolute.filename()), "pak01_dir.vpk")) {
        throw InvalidOperation(kGameArchiveMessage);
    }
    if (!reject_game_folder) {
        return;
    }
    const std::filesystem::path parent = absolute.parent_path();
    if (!parent.empty() && std::filesystem::exists(parent / "pak01_dir.vpk")) {
        throw InvalidOperation(kGameFolderMessage);
    }
}

bool IsVpkFileName(const std::filesystem::path& path) {
    return EqualsIgnoreCase(PathText(path.extension()), ".vpk");
}

bool IsGameArchiveName(const std::filesystem::path& path) {
    return EqualsIgnoreCase(PathText(path.filename()), "pak01_dir.vpk");
}

std::string KindFor(std::string_view path_key) {
    static constexpr std::string_view kKinds[] = {
        "vmdl_c",
        "vnmskel_c",
        "vmat_c",
        "vtex_c",
        "vmesh_c",
        "vnmclip_c",
    };
    for (const std::string_view kind : kKinds) {
        if (path_key.size() > kind.size() && path_key[path_key.size() - kind.size() - 1] == '.'
            && path_key.ends_with(kind)) {
            return std::string(kind);
        }
    }
    return "other";
}

std::optional<std::array<uint8_t, 32>> DataHash(std::span<const uint8_t> payload) {
    if (payload.size() < 16) {
        return std::nullopt;
    }
    uint16_t version = 0;
    std::memcpy(&version, payload.data() + 4, sizeof(version));
    if (version != 12) {
        return std::nullopt;
    }
    try {
        const std::span<const uint8_t> data = FindDataBlock(payload);
        return Sha256(data);
    } catch (const InvalidData&) {
        return std::nullopt;
    }
}

void AddUnique(std::vector<std::string>& names, std::string name) {
    if (name.empty()) {
        return;
    }
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(std::move(name));
    }
}

std::vector<std::string> ReferenceNames(std::string_view kind, std::span<const uint8_t> payload) {
    std::vector<std::string> names;
    if (kind != "vmdl_c" && kind != "vnmskel_c") {
        return names;
    }
    try {
        for (const std::string& name : ExternalReferenceNames(payload)) {
            AddUnique(names, name);
        }
    } catch (const InvalidData&) {
    }
    if (kind != "vmdl_c") {
        return names;
    }
    try {
        const std::span<const uint8_t> data = FindDataBlock(payload);
        if (data.size() < 4) {
            return names;
        }
        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), sizeof(magic));
        if (!IsBinaryKv3(magic)) {
            return names;
        }
        const KvDocument document = ReadKv3(data);
        const KvValue* refs = document.Root.Find("m_vecNmSkeletonRefs");
        if (refs == nullptr || !refs->IsArray()) {
            return names;
        }
        for (const KvValue& item : refs->Items()) {
            if (item.Kind != KvKind::String) {
                continue;
            }
            AddUnique(names, VpkArchive::Normalize(item.String() + "_c"));
        }
    } catch (const InvalidData&) {
    }
    return names;
}

struct PreparedEntry {
    std::string path;
    std::string path_key;
    std::string kind;
    uint32_t crc = 0;
    std::array<uint8_t, 32> sha{};
    std::string sha_hex;
    std::optional<std::array<uint8_t, 32>> data_sha;
    int64_t size = 0;
    std::vector<std::string> refs;
};

struct RefRow {
    std::string from_path_key;
    std::string name;
    std::optional<std::string> resolved_path_key;
    std::optional<std::array<uint8_t, 32>> resolved_blob;
};

std::vector<RefRow> ResolveRefs(const std::vector<PreparedEntry>& entries) {
    std::unordered_map<std::string, std::array<uint8_t, 32>> by_key;
    for (const PreparedEntry& entry : entries) {
        by_key.emplace(entry.path_key, entry.sha);
    }

    std::vector<RefRow> rows;
    std::unordered_set<std::string> seen;
    for (const PreparedEntry& entry : entries) {
        for (const std::string& name : entry.refs) {
            if (!seen.insert(entry.path_key + "\n" + name).second) {
                continue;
            }
            RefRow row;
            row.from_path_key = entry.path_key;
            row.name = name;
            const std::string normalized = AsciiLower(VpkArchive::Normalize(name));
            if (const auto found = by_key.find(normalized); found != by_key.end()) {
                row.resolved_path_key = normalized;
                row.resolved_blob = found->second;
            } else if (const auto compiled = by_key.find(normalized + "_c"); compiled != by_key.end()) {
                row.resolved_path_key = normalized + "_c";
                row.resolved_blob = compiled->second;
            }
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

std::string ManifestBody(const std::vector<PreparedEntry>& entries) {
    std::string body = "dmu-manifest-v1\n";
    for (const PreparedEntry& entry : entries) {
        body += entry.path_key;
        body += '\t';
        body += entry.sha_hex;
        body += '\t';
        body += std::to_string(entry.crc);
        body += '\t';
        body += entry.kind;
        body += '\n';
    }
    return body;
}

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() { reset(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

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

std::string Win32Message(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = "File error " + std::to_string(code);
    if (length != 0 && buffer != nullptr) {
        DWORD end = length;
        while (end > 0 && (buffer[end - 1] == L'\r' || buffer[end - 1] == L'\n')) {
            buffer[end - 1] = L'\0';
            --end;
        }
        text += ": " + Narrow(buffer);
        LocalFree(buffer);
    }
    return text;
}

void WriteExact(HANDLE handle, const uint8_t* data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written, std::numeric_limits<DWORD>::max()));
        DWORD wrote = 0;
        if (!WriteFile(handle, data + written, chunk, &wrote, nullptr) || wrote == 0) {
            throw IoError("Could not write a model blob. " + Win32Message(GetLastError()));
        }
        written += wrote;
    }
}

std::vector<uint8_t> ReadExactFile(const std::filesystem::path& path) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        throw IoError("Could not read a model blob. " + Win32Message(GetLastError()));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) {
        throw IoError("Could not read a model blob. " + Win32Message(GetLastError()));
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t read_total = 0;
    while (read_total < bytes.size()) {
        const DWORD ask = static_cast<DWORD>(
            std::min<size_t>(bytes.size() - read_total, std::numeric_limits<DWORD>::max()));
        DWORD got = 0;
        if (!ReadFile(file.get(), bytes.data() + read_total, ask, &got, nullptr)) {
            throw IoError("Could not read a model blob. " + Win32Message(GetLastError()));
        }
        if (got == 0) {
            throw IoError("Could not read a model blob.");
        }
        read_total += got;
    }
    return bytes;
}

std::filesystem::path BlobPath(const std::filesystem::path& store, std::string_view hex) {
    return store / "blobs" / "sha256" / std::string(hex.substr(0, 2)) / std::string(hex);
}

void WriteBlobFile(const std::filesystem::path& store, std::string_view hex, const std::vector<uint8_t>& payload) {
    const std::filesystem::path final_path = BlobPath(store, hex);
    if (std::filesystem::exists(final_path)) {
        return;
    }
    const std::filesystem::path incoming = store / "incoming";
    try {
        std::filesystem::create_directories(incoming);
        std::filesystem::create_directories(final_path.parent_path());
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not write a model blob. ") + error.what());
    }
    const std::filesystem::path temp = incoming / NewUuid();

    FileHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        throw IoError("Could not write a model blob. " + Win32Message(GetLastError()));
    }
    try {
        if (!payload.empty()) {
            WriteExact(file.get(), payload.data(), payload.size());
        }
        if (!FlushFileBuffers(file.get())) {
            throw IoError("Could not write a model blob. " + Win32Message(GetLastError()));
        }
    } catch (...) {
        file.reset();
        DeleteFileW(temp.c_str());
        throw;
    }
    file.reset();

    if (std::filesystem::exists(final_path)) {
        DeleteFileW(temp.c_str());
        return;
    }
    if (!MoveFileW(temp.c_str(), final_path.c_str())) {
        DeleteFileW(temp.c_str());
        if (std::filesystem::exists(final_path)) {
            return;
        }
        throw IoError("Could not write a model blob. " + Win32Message(GetLastError()));
    }
}

bool BlobRowExists(sqlite3* db, const std::array<uint8_t, 32>& sha) {
    Statement stmt(db, "SELECT 1 FROM blobs WHERE sha256 = ?");
    stmt.BindBlob(1, sha);
    return stmt.Step();
}

std::string LineageForExistingBlob(sqlite3* db, const std::array<uint8_t, 32>& sha) {
    {
        Statement stmt(db, "SELECT lineage_id FROM memberships WHERE blob_sha256 = ? LIMIT 1");
        stmt.BindBlob(1, sha);
        if (stmt.Step()) {
            const std::string id = stmt.ColumnText(0);
            if (!id.empty()) {
                return id;
            }
        }
    }
    Statement stmt(db, "SELECT id FROM lineages WHERE root_sha256 = ? LIMIT 1");
    stmt.BindBlob(1, sha);
    if (stmt.Step()) {
        const std::string id = stmt.ColumnText(0);
        if (!id.empty()) {
            return id;
        }
    }
    throw InvalidData("The model library is missing a lineage for an imported blob.");
}

void InsertBlob(sqlite3* db, const PreparedEntry& entry, const std::string& created_utc) {
    Statement stmt(db,
        "INSERT INTO blobs (sha256, size, codec, stored_size, data_sha256, bytes_state, created_utc) "
        "VALUES (?, ?, 'none', ?, ?, 'present', ?)");
    stmt.BindBlob(1, entry.sha);
    stmt.BindInt64(2, entry.size);
    stmt.BindInt64(3, entry.size);
    if (entry.data_sha) {
        stmt.BindBlob(4, *entry.data_sha);
    } else {
        stmt.BindNull(4);
    }
    stmt.BindText(5, created_utc);
    stmt.Execute();
}

void InsertLineage(sqlite3* db, const std::string& id, const std::array<uint8_t, 32>& sha, const std::string& created_utc) {
    Statement stmt(db, "INSERT INTO lineages (id, root_sha256, created_utc) VALUES (?, ?, ?)");
    stmt.BindText(1, id);
    stmt.BindBlob(2, sha);
    stmt.BindText(3, created_utc);
    stmt.Execute();
}

std::string CommitImport(sqlite3* db, const std::vector<PreparedEntry>& entries, const std::vector<RefRow>& refs,
    const std::array<uint8_t, 32>& manifest_sha, const std::string& absolute_path,
    const std::array<uint8_t, 32>& container_sha, int64_t file_size, CancellationToken cancellation) {
    const std::string now = UtcNow();
    Exec(db, "BEGIN IMMEDIATE;");
    try {
        std::unordered_map<std::string, std::string> lineages;
        for (const PreparedEntry& entry : entries) {
            if (lineages.contains(entry.sha_hex)) {
                continue;
            }
            if (BlobRowExists(db, entry.sha)) {
                lineages.emplace(entry.sha_hex, LineageForExistingBlob(db, entry.sha));
            } else {
                const std::string lineage = NewId();
                InsertBlob(db, entry, now);
                InsertLineage(db, lineage, entry.sha, now);
                lineages.emplace(entry.sha_hex, lineage);
            }
        }

        std::string snapshot_id;
        bool created = false;
        {
            Statement stmt(db, "SELECT id FROM snapshots WHERE manifest_sha256 = ?");
            stmt.BindBlob(1, manifest_sha);
            if (stmt.Step()) {
                snapshot_id = stmt.ColumnText(0);
            }
        }
        if (snapshot_id.empty()) {
            snapshot_id = NewId();
            created = true;
            Statement stmt(db, "INSERT INTO snapshots (id, manifest_sha256, created_utc, tool_version) VALUES (?, ?, ?, NULL)");
            stmt.BindText(1, snapshot_id);
            stmt.BindBlob(2, manifest_sha);
            stmt.BindText(3, now);
            stmt.Execute();

            for (const PreparedEntry& entry : entries) {
                Statement membership(db,
                    "INSERT INTO memberships (snapshot_id, path, path_key, blob_sha256, crc32, kind, lineage_id) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
                membership.BindText(1, snapshot_id);
                membership.BindText(2, entry.path);
                membership.BindText(3, entry.path_key);
                membership.BindBlob(4, entry.sha);
                membership.BindInt64(5, static_cast<int64_t>(entry.crc));
                membership.BindText(6, entry.kind);
                membership.BindText(7, lineages.at(entry.sha_hex));
                membership.Execute();
            }
            for (const RefRow& ref : refs) {
                Statement row(db,
                    "INSERT INTO refs (snapshot_id, from_path_key, name, resolved_path_key, resolved_blob) "
                    "VALUES (?, ?, ?, ?, ?)");
                row.BindText(1, snapshot_id);
                row.BindText(2, ref.from_path_key);
                row.BindText(3, ref.name);
                if (ref.resolved_path_key && ref.resolved_blob) {
                    row.BindText(4, *ref.resolved_path_key);
                    row.BindBlob(5, *ref.resolved_blob);
                } else {
                    row.BindNull(4);
                    row.BindNull(5);
                }
                row.Execute();
            }
        }

        std::string observation_id;
        if (!created) {
            Statement stmt(db,
                "SELECT id FROM observations WHERE snapshot_id = ? AND absolute_path = ? AND container_sha256 = ? LIMIT 1");
            stmt.BindText(1, snapshot_id);
            stmt.BindText(2, absolute_path);
            stmt.BindBlob(3, container_sha);
            if (stmt.Step()) {
                observation_id = stmt.ColumnText(0);
            }
        }
        if (!observation_id.empty()) {
            Statement stmt(db, "UPDATE observations SET observed_utc = ? WHERE id = ?");
            stmt.BindText(1, now);
            stmt.BindText(2, observation_id);
            stmt.Execute();
        } else {
            Statement stmt(db,
                "INSERT INTO observations (id, snapshot_id, absolute_path, container_sha256, file_size, observed_utc, source) "
                "VALUES (?, ?, ?, ?, ?, ?, 'import')");
            stmt.BindText(1, NewId());
            stmt.BindText(2, snapshot_id);
            stmt.BindText(3, absolute_path);
            stmt.BindBlob(4, container_sha);
            stmt.BindInt64(5, file_size);
            stmt.BindText(6, now);
            stmt.Execute();
        }

        cancellation.ThrowIfCancelled();
        Exec(db, "COMMIT;");
        return snapshot_id;
    } catch (...) {
        Rollback(db);
        throw;
    }
}

void ApplyPackLabel(const std::string& snapshot_id, bool path_missing, std::string absolute_path, std::string observed_utc,
    std::string& label, std::string& utc_out) {
    const auto slash = absolute_path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? absolute_path : absolute_path.substr(slash + 1);
    if (path_missing || file.empty()) {
        label = snapshot_id;
        utc_out.clear();
        return;
    }
    label = std::move(file);
    utc_out = std::move(observed_utc);
}

std::vector<ModelRow> LoadModelRows(sqlite3* db) {
    Statement stmt(db, R"sql(
SELECT
  m.blob_sha256,
  (
    SELECT d.path
    FROM memberships d
    WHERE d.blob_sha256 = m.blob_sha256 AND d.kind = 'vmdl_c'
    ORDER BY length(d.path), d.path
    LIMIT 1
  ),
  COUNT(DISTINCT m.snapshot_id),
  COUNT(DISTINCT m.path_key),
  MIN(m.lineage_id)
FROM memberships m
WHERE m.kind = 'vmdl_c'
GROUP BY m.blob_sha256
ORDER BY 2, hex(m.blob_sha256)
)sql");
    std::vector<ModelRow> rows;
    while (stmt.Step()) {
        ModelRow row;
        row.Blob = stmt.ColumnBlobId(0);
        row.DisplayPath = stmt.ColumnText(1);
        row.SnapshotCount = static_cast<int>(stmt.ColumnInt64(2));
        row.PathCount = static_cast<int>(stmt.ColumnInt64(3));
        row.LineageId = stmt.ColumnText(4);
        rows.push_back(std::move(row));
    }
    return rows;
}

struct InputMember {
    std::string path;
    std::string path_key;
    std::array<uint8_t, 32> sha{};
    std::optional<std::array<uint8_t, 32>> data_sha;
    uint32_t crc = 0;
    std::string kind;
    std::string lineage_id;
};

struct OutputSlot {
    PreparedEntry entry;
    std::string lineage_id;
    bool same_bytes = true;
    std::vector<uint8_t> payload;
    bool has_payload = false;
};

struct PlannedEdge {
    std::string relation;
    std::array<uint8_t, 32> input_blob{};
    std::string input_path_key;
    std::optional<std::array<uint8_t, 32>> input_data_sha;
    std::array<uint8_t, 32> output_blob{};
    std::string output_path_key;
    std::optional<std::array<uint8_t, 32>> output_data_sha;
    std::string lineage_id;
    std::string detail_json;
};

struct RepairPlan {
    std::vector<OutputSlot> slots;
    std::unordered_map<std::string, std::vector<uint8_t>> payloads;
    std::vector<PlannedEdge> edges;
    std::vector<RefRow> refs;
    std::array<uint8_t, 32> manifest_sha{};
    std::string summary_json;
    std::string input_snapshot_id;
};

bool SnapshotExists(sqlite3* db, const std::string& snapshot_id) {
    Statement stmt(db, "SELECT 1 FROM snapshots WHERE id = ?");
    stmt.BindText(1, snapshot_id);
    return stmt.Step();
}

std::vector<InputMember> LoadMembers(sqlite3* db, const std::string& snapshot_id) {
    Statement stmt(db,
        "SELECT m.path, m.path_key, m.blob_sha256, m.crc32, m.kind, m.lineage_id, b.data_sha256 "
        "FROM memberships m JOIN blobs b ON b.sha256 = m.blob_sha256 WHERE m.snapshot_id = ?");
    stmt.BindText(1, snapshot_id);
    std::vector<InputMember> members;
    while (stmt.Step()) {
        InputMember member;
        member.path = stmt.ColumnText(0);
        member.path_key = stmt.ColumnText(1);
        member.sha = stmt.ColumnBlobId(2).Bytes;
        member.crc = static_cast<uint32_t>(stmt.ColumnInt64(3));
        member.kind = stmt.ColumnText(4);
        member.lineage_id = stmt.ColumnText(5);
        if (!stmt.ColumnNull(6)) {
            member.data_sha = stmt.ColumnBlobId(6).Bytes;
        }
        members.push_back(std::move(member));
    }
    return members;
}

nlohmann::json NoteBody(const RepairModelNote& note) {
    nlohmann::json body = nlohmann::json::object();
    body["outcome"] = note.Outcome;
    body["detail"] = note.Detail;
    body["warnings"] = note.Warnings;
    body["bone_renames"] = nlohmann::json::array();
    for (const auto& rename : note.BoneRenames) {
        body["bone_renames"].push_back(nlohmann::json::array({rename.first, rename.second}));
    }
    if (!note.GamePath.empty()) {
        body["game_path"] = note.GamePath;
    }
    if (note.GameSha256) {
        body["game_sha256"] = ToLowerHex(*note.GameSha256);
    }
    return body;
}

OutputSlot SlotFromMember(const InputMember& member) {
    OutputSlot slot;
    slot.entry.path = member.path;
    slot.entry.path_key = member.path_key;
    slot.entry.kind = member.kind;
    slot.entry.crc = member.crc;
    slot.entry.sha = member.sha;
    slot.entry.sha_hex = ToLowerHex(member.sha);
    slot.lineage_id = member.lineage_id;
    slot.same_bytes = true;
    return slot;
}

OutputSlot SlotFromWrite(const InputMember& input, const std::string& output_path, const std::string& output_key,
    const std::vector<uint8_t>& bytes) {
    OutputSlot slot;
    slot.entry.path = output_path;
    slot.entry.path_key = output_key;
    slot.entry.kind = KindFor(output_key);
    slot.entry.crc = VpkArchive::Crc(bytes);
    slot.entry.sha = Sha256(bytes);
    slot.entry.sha_hex = ToLowerHex(slot.entry.sha);
    slot.entry.size = static_cast<int64_t>(bytes.size());
    slot.entry.data_sha = DataHash(bytes);
    slot.entry.refs = ReferenceNames(slot.entry.kind, bytes);
    slot.lineage_id = input.lineage_id;
    slot.same_bytes = slot.entry.sha == input.sha;
    slot.payload = bytes;
    slot.has_payload = true;
    return slot;
}

RepairPlan BuildRepairPlan(sqlite3* db, const std::filesystem::path& directory, const RepairRequest& repair) {
    const std::vector<InputMember> members = LoadMembers(db, repair.Input.Value);
    std::unordered_map<std::string, InputMember> inputs;
    std::unordered_map<std::string, OutputSlot> slots;
    for (const InputMember& member : members) {
        inputs.emplace(member.path_key, member);
        slots.emplace(member.path_key, SlotFromMember(member));
    }

    struct WriteOp {
        InputMember input;
        std::string output_path;
        std::string output_key;
        std::vector<uint8_t> bytes;
    };
    std::vector<WriteOp> writes;
    std::vector<std::string> removals;
    for (const RepairChange& change : repair.Changes) {
        const std::string input_path = VpkArchive::Normalize(change.InputPath);
        const std::string input_key = AsciiLower(input_path);
        const bool has_bytes = change.Bytes.has_value();
        const bool has_output = !VpkArchive::Normalize(change.OutputPath).empty();
        if (has_bytes != has_output) {
            throw InvalidData("A repair change needs an output path and bytes, or neither for a removal.");
        }
        const auto found = inputs.find(input_key);
        if (found == inputs.end()) {
            throw InvalidData("The repair names a path that is not in the input snapshot.");
        }
        if (!has_bytes) {
            removals.push_back(input_key);
            continue;
        }
        WriteOp write;
        write.input = found->second;
        write.output_path = VpkArchive::Normalize(change.OutputPath);
        write.output_key = AsciiLower(write.output_path);
        write.bytes = *change.Bytes;
        writes.push_back(std::move(write));
    }

    std::unordered_set<std::string> written_keys;
    for (const WriteOp& write : writes) {
        if (!written_keys.insert(write.output_key).second) {
            throw InvalidData("VPK contains duplicate resource paths.");
        }
    }
    for (const std::string& removed : removals) {
        if (!written_keys.contains(removed)) {
            slots.erase(removed);
        }
    }
    for (const WriteOp& write : writes) {
        if (write.output_key != write.input.path_key && !written_keys.contains(write.input.path_key)) {
            slots.erase(write.input.path_key);
        }
        slots.insert_or_assign(write.output_key, SlotFromWrite(write.input, write.output_path, write.output_key, write.bytes));
    }

    nlohmann::json model_notes = nlohmann::json::array();
    std::unordered_map<std::string, std::string> note_by_key;
    for (const RepairModelNote& note : repair.Models) {
        const std::string key = AsciiLower(VpkArchive::Normalize(note.InputPath));
        const nlohmann::json body = NoteBody(note);
        note_by_key.insert_or_assign(key, body.dump());
        model_notes.push_back(body);
    }
    nlohmann::json summary = nlohmann::json::object();
    summary["output_path"] = repair.OutputPath;
    summary["backup_path"] = repair.BackupPath ? nlohmann::json(*repair.BackupPath) : nlohmann::json(nullptr);
    summary["models"] = std::move(model_notes);

    RepairPlan plan;
    plan.input_snapshot_id = repair.Input.Value;
    plan.summary_json = summary.dump();
    plan.slots.reserve(slots.size());
    for (auto& item : slots) {
        plan.slots.push_back(std::move(item.second));
    }
    std::sort(plan.slots.begin(), plan.slots.end(), [](const OutputSlot& left, const OutputSlot& right) {
        return left.entry.path_key < right.entry.path_key;
    });
    for (OutputSlot& slot : plan.slots) {
        if (slot.has_payload || (slot.entry.kind != "vmdl_c" && slot.entry.kind != "vnmskel_c")) {
            continue;
        }
        slot.entry.refs = ReferenceNames(slot.entry.kind, ReadExactFile(BlobPath(directory, slot.entry.sha_hex)));
    }

    std::vector<PreparedEntry> prepared;
    prepared.reserve(plan.slots.size());
    for (const OutputSlot& slot : plan.slots) {
        prepared.push_back(slot.entry);
        if (!slot.same_bytes && slot.has_payload) {
            plan.payloads.emplace(slot.entry.sha_hex, slot.payload);
        }
    }
    plan.refs = ResolveRefs(prepared);
    const std::string manifest = ManifestBody(prepared);
    plan.manifest_sha = Sha256(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()));

    for (const WriteOp& write : writes) {
        const std::array<uint8_t, 32> sha = Sha256(write.bytes);
        const bool same_bytes = sha == write.input.sha;
        const bool same_path = write.output_key == write.input.path_key;
        if (same_bytes && same_path) {
            continue;
        }
        PlannedEdge edge;
        if (same_bytes) {
            edge.relation = "renamed";
        } else if (same_path) {
            edge.relation = "mutated";
        } else {
            edge.relation = "renamed+mutated";
        }
        edge.input_blob = write.input.sha;
        edge.input_path_key = write.input.path_key;
        edge.input_data_sha = write.input.data_sha;
        edge.output_blob = sha;
        edge.output_path_key = write.output_key;
        edge.output_data_sha = DataHash(write.bytes);
        edge.lineage_id = write.input.lineage_id;
        if (const auto note = note_by_key.find(write.input.path_key); note != note_by_key.end()) {
            edge.detail_json = note->second;
        } else {
            edge.detail_json = "{}";
        }
        plan.edges.push_back(std::move(edge));
    }
    return plan;
}

void BindOptionalHash(Statement& stmt, int index, const std::optional<std::array<uint8_t, 32>>& hash) {
    if (hash) {
        stmt.BindBlob(index, *hash);
    } else {
        stmt.BindNull(index);
    }
}

std::string CommitRepair(sqlite3* db, const RepairPlan& plan, CancellationToken cancellation) {
    const std::string now = UtcNow();
    Exec(db, "BEGIN IMMEDIATE;");
    try {
        std::unordered_set<std::string> existed;
        for (const OutputSlot& slot : plan.slots) {
            if (BlobRowExists(db, slot.entry.sha)) {
                existed.insert(slot.entry.sha_hex);
            } else {
                InsertBlob(db, slot.entry, now);
            }
        }

        std::string snapshot_id;
        {
            Statement stmt(db, "SELECT id FROM snapshots WHERE manifest_sha256 = ?");
            stmt.BindBlob(1, plan.manifest_sha);
            if (stmt.Step()) {
                snapshot_id = stmt.ColumnText(0);
            }
        }
        if (snapshot_id.empty()) {
            snapshot_id = NewId();
            Statement stmt(db, "INSERT INTO snapshots (id, manifest_sha256, created_utc, tool_version) VALUES (?, ?, ?, NULL)");
            stmt.BindText(1, snapshot_id);
            stmt.BindBlob(2, plan.manifest_sha);
            stmt.BindText(3, now);
            stmt.Execute();

            for (const OutputSlot& slot : plan.slots) {
                std::string lineage = slot.lineage_id;
                if (!slot.same_bytes && existed.contains(slot.entry.sha_hex)) {
                    lineage = LineageForExistingBlob(db, slot.entry.sha);
                }
                Statement membership(db,
                    "INSERT INTO memberships (snapshot_id, path, path_key, blob_sha256, crc32, kind, lineage_id) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
                membership.BindText(1, snapshot_id);
                membership.BindText(2, slot.entry.path);
                membership.BindText(3, slot.entry.path_key);
                membership.BindBlob(4, slot.entry.sha);
                membership.BindInt64(5, static_cast<int64_t>(slot.entry.crc));
                membership.BindText(6, slot.entry.kind);
                membership.BindText(7, lineage);
                membership.Execute();
            }
            for (const RefRow& ref : plan.refs) {
                Statement row(db,
                    "INSERT INTO refs (snapshot_id, from_path_key, name, resolved_path_key, resolved_blob) "
                    "VALUES (?, ?, ?, ?, ?)");
                row.BindText(1, snapshot_id);
                row.BindText(2, ref.from_path_key);
                row.BindText(3, ref.name);
                if (ref.resolved_path_key && ref.resolved_blob) {
                    row.BindText(4, *ref.resolved_path_key);
                    row.BindBlob(5, *ref.resolved_blob);
                } else {
                    row.BindNull(4);
                    row.BindNull(5);
                }
                row.Execute();
            }
        }

        const std::string transform_id = NewId();
        {
            Statement stmt(db,
                "INSERT INTO transforms (id, kind, tool_name, tool_version, finished_utc, summary_json, "
                "input_snapshot_id, output_snapshot_id) VALUES (?, 'repair', 'HeroModelUpdater', '1', ?, ?, ?, ?)");
            stmt.BindText(1, transform_id);
            stmt.BindText(2, now);
            stmt.BindText(3, plan.summary_json);
            stmt.BindText(4, plan.input_snapshot_id);
            stmt.BindText(5, snapshot_id);
            stmt.Execute();
        }
        for (const PlannedEdge& edge : plan.edges) {
            Statement stmt(db,
                "INSERT INTO edges (transform_id, relation, input_blob, input_path_key, input_data_sha256, "
                "output_blob, output_path_key, output_data_sha256, lineage_id, detail_json) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
            stmt.BindText(1, transform_id);
            stmt.BindText(2, edge.relation);
            stmt.BindBlob(3, edge.input_blob);
            stmt.BindText(4, edge.input_path_key);
            BindOptionalHash(stmt, 5, edge.input_data_sha);
            stmt.BindBlob(6, edge.output_blob);
            stmt.BindText(7, edge.output_path_key);
            BindOptionalHash(stmt, 8, edge.output_data_sha);
            stmt.BindText(9, edge.lineage_id);
            stmt.BindText(10, edge.detail_json);
            stmt.Execute();
        }
        cancellation.ThrowIfCancelled();
        Exec(db, "COMMIT;");
        return snapshot_id;
    } catch (...) {
        Rollback(db);
        throw;
    }
}

std::string LineageOfBlob(sqlite3* db, const std::array<uint8_t, 32>& sha) {
    {
        Statement stmt(db,
            "SELECT lineage_id FROM memberships WHERE blob_sha256 = ? AND lineage_id != '' ORDER BY lineage_id LIMIT 1");
        stmt.BindBlob(1, sha);
        if (stmt.Step()) {
            return stmt.ColumnText(0);
        }
    }
    {
        Statement stmt(db,
            "SELECT lineage_id FROM edges WHERE (input_blob = ? OR output_blob = ?) AND lineage_id != '' "
            "ORDER BY lineage_id LIMIT 1");
        stmt.BindBlob(1, sha);
        stmt.BindBlob(2, sha);
        if (stmt.Step()) {
            return stmt.ColumnText(0);
        }
    }
    Statement stmt(db, "SELECT id FROM lineages WHERE root_sha256 = ? ORDER BY id LIMIT 1");
    stmt.BindBlob(1, sha);
    if (stmt.Step()) {
        return stmt.ColumnText(0);
    }
    return {};
}

std::string EdgeDetail(const std::string& detail_json) {
    if (detail_json.empty()) {
        return {};
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(detail_json);
        if (parsed.is_object()) {
            const auto field = parsed.find("detail");
            if (field != parsed.end() && field->is_string()) {
                return field->get<std::string>();
            }
        }
        return {};
    } catch (const nlohmann::json::exception&) {
        return detail_json;
    }
}

constexpr const char* kLineageBlobs = R"sql(
SELECT blob_sha256 FROM memberships WHERE lineage_id = ?
UNION
SELECT input_blob FROM edges WHERE lineage_id = ? AND input_blob IS NOT NULL
UNION
SELECT output_blob FROM edges WHERE lineage_id = ? AND output_blob IS NOT NULL
UNION
SELECT root_sha256 FROM lineages WHERE id = ?
)sql";

void BindLineage(Statement& stmt, int first, const std::string& lineage_id) {
    stmt.BindText(first, lineage_id);
    stmt.BindText(first + 1, lineage_id);
    stmt.BindText(first + 2, lineage_id);
    stmt.BindText(first + 3, lineage_id);
}

std::vector<HistoryVersion> LoadHistoryVersions(sqlite3* db, const std::string& lineage_id) {
    std::unordered_map<std::string, HistoryVersion> versions;
    {
        const std::string sql = std::string(
            "SELECT b.sha256, b.data_sha256, b.created_utc FROM blobs b WHERE b.sha256 IN (")
            + kLineageBlobs + ")";
        Statement stmt(db, sql.c_str());
        BindLineage(stmt, 1, lineage_id);
        while (stmt.Step()) {
            HistoryVersion version;
            version.Blob = stmt.ColumnBlobId(0);
            if (!stmt.ColumnNull(1)) {
                version.Data = stmt.ColumnBlobId(1);
            }
            version.FirstSeenUtc = stmt.ColumnText(2);
            const std::string hex = version.Blob.Hex();
            versions.insert_or_assign(hex, std::move(version));
        }
    }
    {
        Statement stmt(db,
            "SELECT blob_sha256, COUNT(DISTINCT snapshot_id) FROM memberships WHERE lineage_id = ? GROUP BY blob_sha256");
        stmt.BindText(1, lineage_id);
        while (stmt.Step()) {
            const auto found = versions.find(stmt.ColumnBlobId(0).Hex());
            if (found != versions.end()) {
                found->second.SnapshotCount = static_cast<int>(stmt.ColumnInt64(1));
            }
        }
    }
    {
        Statement stmt(db, R"sql(
SELECT m.blob_sha256, m.path
FROM memberships m
JOIN snapshots s ON s.id = m.snapshot_id
WHERE m.lineage_id = ?
GROUP BY m.blob_sha256, m.path
ORDER BY MIN(s.created_utc), m.path
)sql");
        stmt.BindText(1, lineage_id);
        while (stmt.Step()) {
            const auto found = versions.find(stmt.ColumnBlobId(0).Hex());
            if (found != versions.end()) {
                found->second.Paths.push_back(stmt.ColumnText(1));
            }
        }
    }
    std::vector<HistoryVersion> rows;
    rows.reserve(versions.size());
    for (auto& item : versions) {
        rows.push_back(std::move(item.second));
    }
    return rows;
}

bool VersionBefore(const HistoryVersion& left, const HistoryVersion& right) {
    if (left.FirstSeenUtc != right.FirstSeenUtc) {
        return left.FirstSeenUtc < right.FirstSeenUtc;
    }
    return left.Blob.Hex() < right.Blob.Hex();
}

// A renamed edge keeps one blob, so it is a link and not an ordering edge.
std::vector<HistoryVersion> OrderVersions(std::vector<HistoryVersion> versions, const std::vector<HistoryLink>& links) {
    std::unordered_map<std::string, size_t> index;
    index.reserve(versions.size());
    for (size_t i = 0; i < versions.size(); ++i) {
        index.emplace(versions[i].Blob.Hex(), i);
    }
    std::vector<int> indegree(versions.size(), 0);
    std::vector<std::vector<size_t>> next(versions.size());
    for (const HistoryLink& link : links) {
        if (link.From.Bytes == link.To.Bytes) {
            continue;
        }
        const auto from = index.find(link.From.Hex());
        const auto to = index.find(link.To.Hex());
        if (from == index.end() || to == index.end()) {
            continue;
        }
        next[from->second].push_back(to->second);
        indegree[to->second] += 1;
    }

    std::vector<size_t> ready;
    for (size_t i = 0; i < versions.size(); ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }
    const auto earlier = [&](size_t left, size_t right) { return VersionBefore(versions[left], versions[right]); };
    std::sort(ready.begin(), ready.end(), earlier);

    std::vector<char> emitted(versions.size(), 0);
    std::vector<size_t> order;
    order.reserve(versions.size());
    while (!ready.empty()) {
        const size_t current = ready.front();
        ready.erase(ready.begin());
        emitted[current] = 1;
        order.push_back(current);
        for (const size_t child : next[current]) {
            indegree[child] -= 1;
            if (indegree[child] == 0) {
                ready.push_back(child);
            }
        }
        std::sort(ready.begin(), ready.end(), earlier);
    }

    std::vector<size_t> rest;
    for (size_t i = 0; i < versions.size(); ++i) {
        if (emitted[i] == 0) {
            rest.push_back(i);
        }
    }
    std::sort(rest.begin(), rest.end(), earlier);
    order.insert(order.end(), rest.begin(), rest.end());

    std::vector<HistoryVersion> ordered;
    ordered.reserve(order.size());
    for (const size_t i : order) {
        ordered.push_back(std::move(versions[i]));
    }
    return ordered;
}

std::vector<HistoryLink> LoadHistoryLinks(sqlite3* db, const std::string& lineage_id) {
    Statement stmt(db, R"sql(
SELECT e.input_blob, e.output_blob, e.relation, e.detail_json, t.tool_version, t.finished_utc
FROM edges e
INNER JOIN transforms t ON t.id = e.transform_id
WHERE e.lineage_id = ?
ORDER BY t.finished_utc, e.relation, e.id
)sql");
    stmt.BindText(1, lineage_id);
    std::vector<HistoryLink> links;
    while (stmt.Step()) {
        HistoryLink link;
        link.From = stmt.ColumnBlobId(0);
        link.To = stmt.ColumnBlobId(1);
        link.Relation = stmt.ColumnText(2);
        link.Detail = EdgeDetail(stmt.ColumnText(3));
        link.ToolVersion = stmt.ColumnText(4);
        link.FinishedUtc = stmt.ColumnText(5);
        links.push_back(std::move(link));
    }
    return links;
}

std::vector<SlotObservation> LoadUnprovenSamePath(sqlite3* db, const std::array<uint8_t, 32>& sha, const std::string& lineage_id) {
    Statement stmt(db, R"sql(
SELECT m.blob_sha256, m.lineage_id, m.path, m.snapshot_id,
  (
    SELECT o.absolute_path
    FROM observations o
    WHERE o.snapshot_id = m.snapshot_id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  ),
  (
    SELECT o.observed_utc
    FROM observations o
    WHERE o.snapshot_id = m.snapshot_id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  )
FROM memberships m
WHERE m.path_key IN (SELECT path_key FROM memberships WHERE blob_sha256 = ?)
  AND hex(m.blob_sha256) != hex(?)
  AND hex(m.blob_sha256) NOT IN (
    SELECT hex(blob_sha256) FROM memberships WHERE lineage_id = ?
    UNION
    SELECT hex(input_blob) FROM edges WHERE lineage_id = ? AND input_blob IS NOT NULL
    UNION
    SELECT hex(output_blob) FROM edges WHERE lineage_id = ? AND output_blob IS NOT NULL
    UNION
    SELECT hex(root_sha256) FROM lineages WHERE id = ?
  )
ORDER BY m.path, hex(m.blob_sha256), m.snapshot_id
)sql");
    stmt.BindBlob(1, sha);
    stmt.BindBlob(2, sha);
    BindLineage(stmt, 3, lineage_id);
    std::vector<SlotObservation> rows;
    while (stmt.Step()) {
        SlotObservation row;
        row.Blob = stmt.ColumnBlobId(0);
        row.LineageId = stmt.ColumnText(1);
        row.Path = stmt.ColumnText(2);
        const std::string snapshot_id = stmt.ColumnText(3);
        const bool path_missing = stmt.ColumnNull(4);
        const std::string absolute_path = stmt.ColumnText(4);
        const std::string observed_utc = stmt.ColumnText(5);
        ApplyPackLabel(snapshot_id, path_missing, absolute_path, observed_utc, row.PackLabel, row.ObservedUtc);
        rows.push_back(std::move(row));
    }
    return rows;
}

void RemoveLibraryFile(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::remove(path, error) || error) {
        throw IoError("Could not remove a model library file. " + PathText(path));
    }
}

bool IsSha256Hex(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

std::unordered_set<std::string> ReferencedBlobHexes(sqlite3* db) {
    Statement stmt(db, R"sql(
SELECT blob_sha256 FROM memberships
UNION
SELECT input_blob FROM edges WHERE input_blob IS NOT NULL
UNION
SELECT output_blob FROM edges WHERE output_blob IS NOT NULL
)sql");
    std::unordered_set<std::string> hexes;
    while (stmt.Step()) {
        hexes.insert(stmt.ColumnBlobId(0).Hex());
    }
    return hexes;
}

int RemoveUnreferencedBlobFiles(const std::filesystem::path& directory, const std::unordered_set<std::string>& referenced) {
    const std::filesystem::path root = directory / "blobs" / "sha256";
    std::error_code exists_error;
    if (!std::filesystem::exists(root, exists_error) || exists_error) {
        return 0;
    }
    std::vector<std::filesystem::path> doomed;
    try {
        for (const std::filesystem::directory_entry& shard : std::filesystem::directory_iterator(root)) {
            if (!shard.is_directory()) {
                continue;
            }
            for (const std::filesystem::directory_entry& file : std::filesystem::directory_iterator(shard.path())) {
                std::error_code error;
                if (!file.is_regular_file(error) || error) {
                    continue;
                }
                const std::string name = AsciiLower(file.path().filename().string());
                if (IsSha256Hex(name) && !referenced.contains(name)) {
                    doomed.push_back(file.path());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not read model blob files. ") + error.what());
    }
    for (const std::filesystem::path& file : doomed) {
        RemoveLibraryFile(file);
    }
    return static_cast<int>(doomed.size());
}

int RemoveIncomingFiles(const std::filesystem::path& directory) {
    const std::filesystem::path incoming = directory / "incoming";
    std::error_code exists_error;
    if (!std::filesystem::exists(incoming, exists_error) || exists_error) {
        return 0;
    }
    std::vector<std::filesystem::path> files;
    try {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(incoming)) {
            std::error_code error;
            if (entry.is_regular_file(error) && !error) {
                files.push_back(entry.path());
            }
        }
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not read incoming model files. ") + error.what());
    }
    for (const std::filesystem::path& file : files) {
        RemoveLibraryFile(file);
    }
    return static_cast<int>(files.size());
}

}  // namespace

std::string BlobId::Hex() const { return ToLowerHex(Bytes); }

struct ContentStore::State {
    std::filesystem::path directory;
    sqlite3* db = nullptr;

    ~State() {
        if (db != nullptr) {
            sqlite3_close_v2(db);
            db = nullptr;
        }
    }

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

ContentStore::ContentStore(std::unique_ptr<State> state) : state_(std::move(state)) {}

ContentStore::ContentStore(ContentStore&&) noexcept = default;

ContentStore& ContentStore::operator=(ContentStore&&) noexcept = default;

ContentStore::~ContentStore() = default;

ContentStore ContentStore::Open(const std::filesystem::path& directory) {
    std::filesystem::path root;
    try {
        root = std::filesystem::absolute(directory).lexically_normal();
        std::filesystem::create_directories(root / "blobs" / "sha256");
        std::filesystem::create_directories(root / "incoming");
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not create the model library. ") + error.what());
    }

    sqlite3* db = nullptr;
    if (sqlite3_open16((root / "catalog.sqlite").c_str(), &db) != SQLITE_OK) {
        std::string message = db != nullptr ? sqlite3_errmsg(db) : "Could not open the model library.";
        if (db != nullptr) {
            sqlite3_close(db);
        }
        throw IoError(message.empty() ? "Could not open the model library." : message);
    }
    try {
        InitializeDatabase(db);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }

    auto state = std::make_unique<State>();
    state->directory = std::move(root);
    state->db = db;
    return ContentStore(std::move(state));
}

SnapshotId ContentStore::ImportVpk(const std::filesystem::path& path, CancellationToken cancellation) {
    return ImportVpkFile(path, std::move(cancellation), true);
}

SnapshotId ContentStore::ImportVpkFile(const std::filesystem::path& path, CancellationToken cancellation, bool reject_game_folder) {
    cancellation.ThrowIfCancelled();
    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(path, exists_error)) {
        throw IoError(kMissingMessage);
    }
    if (IsChunkName(PathText(path.filename()))) {
        throw InvalidOperation(kChunkMessage);
    }

    std::filesystem::path absolute;
    try {
        absolute = std::filesystem::absolute(path).lexically_normal();
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not open the mod VPK. ") + error.what());
    }
    RejectGameArchive(absolute, reject_game_folder);

    const VpkArchive archive = VpkArchive::Open(absolute);
    const auto file_size_raw = std::filesystem::file_size(absolute);
    if (file_size_raw > static_cast<std::uintmax_t>(std::numeric_limits<int64_t>::max())) {
        throw InvalidData("VPK is too large.");
    }
    const int64_t file_size = static_cast<int64_t>(file_size_raw);
    const std::array<uint8_t, 32> container_sha = Sha256File(absolute);
    const std::string absolute_utf8 = PathText(absolute);

    std::vector<PreparedEntry> entries;
    std::unordered_map<std::string, std::vector<uint8_t>> payloads;
    std::unordered_set<std::string> path_keys;
    entries.reserve(archive.Entries().size());
    for (const VpkEntry& entry : archive.Entries()) {
        cancellation.ThrowIfCancelled();
        std::vector<uint8_t> payload = archive.Read(entry, cancellation);
        PreparedEntry prepared;
        prepared.path = VpkArchive::Normalize(entry.FullPath);
        prepared.path_key = AsciiLower(prepared.path);
        if (!path_keys.insert(prepared.path_key).second) {
            throw InvalidData("VPK contains duplicate resource paths.");
        }
        prepared.kind = KindFor(prepared.path_key);
        prepared.crc = entry.Crc32;
        prepared.size = static_cast<int64_t>(payload.size());
        prepared.sha = Sha256(payload);
        prepared.sha_hex = ToLowerHex(prepared.sha);
        prepared.data_sha = DataHash(payload);
        prepared.refs = ReferenceNames(prepared.kind, payload);
        payloads.insert_or_assign(prepared.sha_hex, std::move(payload));
        entries.push_back(std::move(prepared));
    }

    std::sort(entries.begin(), entries.end(), [](const PreparedEntry& left, const PreparedEntry& right) {
        return left.path_key < right.path_key;
    });
    const std::string manifest = ManifestBody(entries);
    const std::array<uint8_t, 32> manifest_sha = Sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()));
    const std::vector<RefRow> refs = ResolveRefs(entries);

    std::unordered_set<std::string> written;
    for (const PreparedEntry& entry : entries) {
        if (!written.insert(entry.sha_hex).second) {
            continue;
        }
        cancellation.ThrowIfCancelled();
        WriteBlobFile(state_->directory, entry.sha_hex, payloads.at(entry.sha_hex));
    }
    payloads.clear();
    cancellation.ThrowIfCancelled();

    SnapshotId id;
    id.Value = CommitImport(state_->db, entries, refs, manifest_sha, absolute_utf8, container_sha, file_size, cancellation);
    return id;
}

std::vector<std::filesystem::path> FolderImportCandidates(const std::filesystem::path& directory, int* skipped) {
    std::error_code directory_error;
    if (!std::filesystem::is_directory(directory, directory_error) || directory_error) {
        throw IoError("Could not find the mod folder.");
    }

    std::vector<std::filesystem::path> files;
    try {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
            std::error_code error;
            if (!entry.is_regular_file(error) || error) {
                continue;
            }
            files.push_back(entry.path());
        }
    } catch (const std::filesystem::filesystem_error& error) {
        throw IoError(std::string("Could not read the mod folder. ") + error.what());
    }

    std::sort(files.begin(), files.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return CompareIgnoreCase(PathText(left.filename()), PathText(right.filename())) < 0;
    });

    std::vector<std::filesystem::path> candidates;
    int skipped_files = 0;
    for (const std::filesystem::path& file : files) {
        if (!IsVpkFileName(file)) {
            continue;
        }
        if (IsChunkName(PathText(file.filename())) || IsGameArchiveName(file)) {
            ++skipped_files;
            continue;
        }
        candidates.push_back(file);
    }
    if (skipped != nullptr) {
        *skipped = skipped_files;
    }
    return candidates;
}

FolderImportResult ContentStore::ImportFolder(const std::filesystem::path& directory, CancellationToken cancellation) {
    cancellation.ThrowIfCancelled();
    FolderImportResult result;
    for (const std::filesystem::path& file : FolderImportCandidates(directory, &result.SkippedFiles)) {
        cancellation.ThrowIfCancelled();
        try {
            // The folder scan already skipped pak01_dir.vpk. Eligible siblings in that folder are mods.
            result.Snapshots.push_back(ImportVpkFile(file, cancellation, false));
        } catch (const OperationCancelled&) {
            throw;
        } catch (const std::exception& error) {
            FolderImportFailure failure;
            failure.Path = file;
            failure.Message = error.what();
            result.Failures.push_back(std::move(failure));
        }
    }
    return result;
}

std::vector<uint8_t> ContentStore::ReadBlob(const BlobId& id) const {
    const std::string hex = id.Hex();
    if (hex.size() != 64) {
        throw InvalidData("The model library has no blob with that id.");
    }
    int64_t size = 0;
    std::string codec;
    {
        Statement stmt(state_->db, "SELECT size, codec FROM blobs WHERE sha256 = ?");
        stmt.BindBlob(1, id.Bytes);
        if (!stmt.Step()) {
            throw InvalidData("The model library has no blob with that id.");
        }
        size = stmt.ColumnInt64(0);
        codec = stmt.ColumnText(1);
    }
    if (codec != "none") {
        throw InvalidData("Unsupported blob codec.");
    }

    std::vector<uint8_t> bytes = ReadExactFile(BlobPath(state_->directory, hex));
    if (static_cast<int64_t>(bytes.size()) != size || Sha256(bytes) != id.Bytes) {
        throw InvalidData("The stored model blob does not match its id.");
    }
    return bytes;
}

std::optional<std::vector<uint8_t>> ContentStore::ReadSnapshotPath(const SnapshotId& snapshot,
    std::string_view path) const {
    const std::string normalized = VpkArchive::Normalize(std::string(path));
    if (snapshot.Value.empty() || normalized.empty()) {
        return std::nullopt;
    }
    BlobId blob;
    {
        Statement stmt(state_->db, "SELECT blob_sha256 FROM memberships WHERE snapshot_id = ? AND path_key = ?");
        stmt.BindText(1, snapshot.Value);
        stmt.BindText(2, AsciiLower(normalized));
        if (!stmt.Step()) {
            return std::nullopt;
        }
        blob = stmt.ColumnBlobId(0);
    }
    return ReadBlob(blob);
}

std::vector<std::string> ContentStore::SnapshotPaths(const SnapshotId& snapshot, std::string_view kind) const {
    Statement stmt(state_->db, "SELECT path FROM memberships WHERE snapshot_id = ? AND kind = ? ORDER BY path");
    stmt.BindText(1, snapshot.Value);
    stmt.BindText(2, kind);
    std::vector<std::string> paths;
    while (stmt.Step()) {
        paths.push_back(stmt.ColumnText(0));
    }
    return paths;
}

std::vector<SnapshotMember> ContentStore::SnapshotMembers(const SnapshotId& snapshot) const {
    Statement stmt(state_->db,
        "SELECT m.path, m.blob_sha256, m.crc32, b.size FROM memberships m JOIN blobs b ON b.sha256 = m.blob_sha256 "
        "WHERE m.snapshot_id = ? ORDER BY m.path_key");
    stmt.BindText(1, snapshot.Value);
    std::vector<SnapshotMember> members;
    while (stmt.Step()) {
        SnapshotMember member;
        member.Path = stmt.ColumnText(0);
        member.Blob = stmt.ColumnBlobId(1);
        member.Crc32 = static_cast<uint32_t>(stmt.ColumnInt64(2));
        member.Size = stmt.ColumnInt64(3);
        members.push_back(std::move(member));
    }
    return members;
}

std::optional<std::string> ContentStore::SnapshotContentId(const SnapshotId& snapshot) const {
    Statement stmt(state_->db, "SELECT manifest_sha256 FROM snapshots WHERE id = ?");
    stmt.BindText(1, snapshot.Value);
    if (!stmt.Step()) {
        return std::nullopt;
    }
    return stmt.ColumnBlobId(0).Hex();
}

std::optional<SnapshotId> ContentStore::FindSnapshot(std::string_view content_id) const {
    if (!IsSha256Hex(content_id)) {
        return std::nullopt;
    }
    std::array<uint8_t, 32> manifest{};
    for (size_t i = 0; i < manifest.size(); ++i) {
        const auto digit = [&](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        manifest[i] = static_cast<uint8_t>(digit(content_id[2 * i]) << 4 | digit(content_id[2 * i + 1]));
    }
    Statement stmt(state_->db, "SELECT id FROM snapshots WHERE manifest_sha256 = ?");
    stmt.BindBlob(1, manifest);
    if (!stmt.Step()) {
        return std::nullopt;
    }
    return SnapshotId{stmt.ColumnText(0)};
}

std::vector<ModelRow> ContentStore::ListModels() const { return LoadModelRows(state_->db); }

std::vector<PackRow> ContentStore::ListPacks() const {
    Statement stmt(state_->db, R"sql(
SELECT
  s.id,
  (SELECT COUNT(*) FROM memberships m WHERE m.snapshot_id = s.id AND m.kind = 'vmdl_c'),
  (SELECT COUNT(*) FROM observations o WHERE o.snapshot_id = s.id),
  (
    SELECT o.absolute_path
    FROM observations o
    WHERE o.snapshot_id = s.id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  ),
  (
    SELECT o.observed_utc
    FROM observations o
    WHERE o.snapshot_id = s.id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  ),
  (
    SELECT o.id
    FROM observations o
    WHERE o.snapshot_id = s.id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  )
FROM snapshots s
ORDER BY 5 DESC, s.id
)sql");
    std::vector<PackRow> rows;
    while (stmt.Step()) {
        PackRow row;
        row.Snapshot.Value = stmt.ColumnText(0);
        row.ModelCount = static_cast<int>(stmt.ColumnInt64(1));
        row.ObservationCount = static_cast<int>(stmt.ColumnInt64(2));
        ApplyPackLabel(row.Snapshot.Value, stmt.ColumnNull(3), stmt.ColumnText(3), stmt.ColumnText(4), row.Label,
            row.ObservedUtc);
        if (!stmt.ColumnNull(3)) {
            row.ObservationPath = stmt.ColumnText(3);
        }
        if (!stmt.ColumnNull(5)) {
            row.ObservationId = stmt.ColumnText(5);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<MembershipRow> ContentStore::Memberships(const BlobId& blob) const {
    Statement stmt(state_->db, R"sql(
SELECT
  m.snapshot_id,
  m.path,
  (
    SELECT o.absolute_path
    FROM observations o
    WHERE o.snapshot_id = m.snapshot_id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  ),
  (
    SELECT o.observed_utc
    FROM observations o
    WHERE o.snapshot_id = m.snapshot_id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  ),
  (
    SELECT o.id
    FROM observations o
    WHERE o.snapshot_id = m.snapshot_id
    ORDER BY o.observed_utc DESC, o.absolute_path DESC
    LIMIT 1
  )
FROM memberships m
WHERE m.blob_sha256 = ?
ORDER BY m.path, m.snapshot_id
)sql");
    stmt.BindBlob(1, blob.Bytes);
    std::vector<MembershipRow> rows;
    while (stmt.Step()) {
        MembershipRow row;
        row.Snapshot.Value = stmt.ColumnText(0);
        row.Path = stmt.ColumnText(1);
        ApplyPackLabel(row.Snapshot.Value, stmt.ColumnNull(2), stmt.ColumnText(2), stmt.ColumnText(3), row.PackLabel,
            row.ObservedUtc);
        if (!stmt.ColumnNull(4)) {
            row.ObservationId = stmt.ColumnText(4);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ModelRow> ContentStore::ExactDuplicateModels() const {
    std::vector<ModelRow> rows = ListModels();
    std::erase_if(rows, [](const ModelRow& row) { return row.SnapshotCount <= 1 && row.PathCount <= 1; });
    return rows;
}

SnapshotId ContentStore::RecordRepair(const RepairRequest& repair, CancellationToken cancellation) {
    cancellation.ThrowIfCancelled();
    if (!SnapshotExists(state_->db, repair.Input.Value)) {
        throw InvalidData("The model library has no snapshot with that id.");
    }
    const RepairPlan plan = BuildRepairPlan(state_->db, state_->directory, repair);
    for (const auto& item : plan.payloads) {
        cancellation.ThrowIfCancelled();
        WriteBlobFile(state_->directory, item.first, item.second);
    }
    cancellation.ThrowIfCancelled();
    SnapshotId id;
    id.Value = CommitRepair(state_->db, plan, cancellation);
    return id;
}

History ContentStore::HistoryOf(const BlobId& blob) const {
    History history;
    history.LineageId = LineageOfBlob(state_->db, blob.Bytes);
    if (history.LineageId.empty()) {
        return history;
    }
    history.Links = LoadHistoryLinks(state_->db, history.LineageId);
    history.Versions = OrderVersions(LoadHistoryVersions(state_->db, history.LineageId), history.Links);
    return history;
}

std::vector<SlotObservation> ContentStore::UnprovenSamePath(const BlobId& blob) const {
    return LoadUnprovenSamePath(state_->db, blob.Bytes, LineageOfBlob(state_->db, blob.Bytes));
}

void ContentStore::DropObservation(std::string_view observation_id) {
    Exec(state_->db, "BEGIN IMMEDIATE;");
    try {
        std::string snapshot_id;
        {
            Statement stmt(state_->db, "SELECT snapshot_id FROM observations WHERE id = ?");
            stmt.BindText(1, observation_id);
            if (!stmt.Step()) {
                throw InvalidData(kMissingObservation);
            }
            snapshot_id = stmt.ColumnText(0);
        }
        {
            Statement stmt(state_->db, "DELETE FROM observations WHERE id = ?");
            stmt.BindText(1, observation_id);
            stmt.Execute();
        }
        int64_t remaining = 0;
        {
            Statement stmt(state_->db, "SELECT COUNT(*) FROM observations WHERE snapshot_id = ?");
            stmt.BindText(1, snapshot_id);
            if (stmt.Step()) {
                remaining = stmt.ColumnInt64(0);
            }
        }
        if (remaining == 0) {
            {
                Statement stmt(state_->db, "DELETE FROM memberships WHERE snapshot_id = ?");
                stmt.BindText(1, snapshot_id);
                stmt.Execute();
            }
            {
                Statement stmt(state_->db, "DELETE FROM refs WHERE snapshot_id = ?");
                stmt.BindText(1, snapshot_id);
                stmt.Execute();
            }
            {
                Statement stmt(state_->db, "DELETE FROM snapshots WHERE id = ?");
                stmt.BindText(1, snapshot_id);
                stmt.Execute();
            }
        }
        Exec(state_->db, "COMMIT;");
    } catch (...) {
        Rollback(state_->db);
        throw;
    }
}

GcStats ContentStore::CollectGarbage() {
    GcStats stats;
    Exec(state_->db, "BEGIN IMMEDIATE;");
    try {
        const std::unordered_set<std::string> referenced = ReferencedBlobHexes(state_->db);
        stats.BlobFilesRemoved = RemoveUnreferencedBlobFiles(state_->directory, referenced);
        Statement stmt(state_->db, R"sql(
DELETE FROM blobs
WHERE NOT EXISTS (SELECT 1 FROM memberships m WHERE m.blob_sha256 = blobs.sha256)
  AND NOT EXISTS (
    SELECT 1 FROM edges e
    WHERE e.input_blob = blobs.sha256 OR e.output_blob = blobs.sha256
  )
)sql");
        stmt.Execute();
        stats.BlobRowsRemoved = sqlite3_changes(state_->db);
        Exec(state_->db, "COMMIT;");
    } catch (...) {
        Rollback(state_->db);
        throw;
    }
    stats.IncomingFilesRemoved = RemoveIncomingFiles(state_->directory);
    return stats;
}

namespace {

void WriteEmptyDirectoryVpk(const std::filesystem::path& path) {
    uint8_t bytes[29] = {};
    bytes[0] = 0x34;
    bytes[1] = 0x12;
    bytes[2] = 0xAA;
    bytes[3] = 0x55;
    bytes[4] = 2;
    bytes[8] = 1;
    FileHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        throw IoError("Could not write the assembled VPK. " + Win32Message(GetLastError()));
    }
    WriteExact(file.get(), bytes, sizeof(bytes));
}

}  // namespace

void ContentStore::Assemble(const SnapshotId& snapshot, const std::vector<BlobId>& models,
    const std::filesystem::path& destination, CancellationToken cancellation) {
    cancellation.ThrowIfCancelled();
    if (snapshot.Value.empty()) {
        throw InvalidData("The model library has no snapshot with that id.");
    }
    if (models.empty()) {
        throw InvalidData("Select a model to assemble.");
    }
    if (destination.empty()) {
        throw IoError("Could not write the assembled VPK.");
    }
    {
        Statement exists(state_->db, "SELECT 1 FROM snapshots WHERE id = ?");
        exists.BindText(1, snapshot.Value);
        if (!exists.Step()) {
            throw InvalidData("The model library has no snapshot with that id.");
        }
    }

    struct Member {
        std::string Path;
        std::string PathKey;
        std::string Kind;
        BlobId Blob;
    };
    std::vector<Member> members;
    {
        Statement stmt(state_->db, "SELECT path, path_key, kind, blob_sha256 FROM memberships WHERE snapshot_id = ?");
        stmt.BindText(1, snapshot.Value);
        while (stmt.Step()) {
            Member member;
            member.Path = stmt.ColumnText(0);
            member.PathKey = stmt.ColumnText(1);
            member.Kind = stmt.ColumnText(2);
            member.Blob = stmt.ColumnBlobId(3);
            members.push_back(std::move(member));
        }
    }

    std::unordered_map<std::string, const Member*> by_key;
    for (const Member& member : members) {
        by_key.emplace(member.PathKey, &member);
    }

    std::unordered_set<std::string> included;
    std::vector<std::string> expand;
    for (const BlobId& model : models) {
        cancellation.ThrowIfCancelled();
        bool found = false;
        for (const Member& member : members) {
            if (member.Kind != "vmdl_c" || member.Blob.Bytes != model.Bytes) {
                continue;
            }
            found = true;
            if (included.insert(member.PathKey).second) {
                expand.push_back(member.PathKey);
            }
        }
        if (!found) {
            throw InvalidData("The selected model is not in this pack.");
        }
    }

    std::vector<std::pair<std::string, std::string>> refs;
    {
        Statement stmt(state_->db,
            "SELECT from_path_key, resolved_path_key FROM refs "
            "WHERE snapshot_id = ? AND resolved_path_key IS NOT NULL");
        stmt.BindText(1, snapshot.Value);
        while (stmt.Step()) {
            refs.emplace_back(stmt.ColumnText(0), stmt.ColumnText(1));
        }
    }
    for (size_t index = 0; index < expand.size(); ++index) {
        cancellation.ThrowIfCancelled();
        for (const auto& [from, resolved] : refs) {
            if (from != expand[index] || !included.insert(resolved).second) {
                continue;
            }
            const auto found = by_key.find(resolved);
            if (found == by_key.end() || found->second->Kind == "vmdl_c") {
                included.erase(resolved);
                continue;
            }
            if (found->second->Kind == "vnmskel_c") {
                expand.push_back(resolved);
            }
        }
    }

    std::vector<VpkChange> changes;
    changes.reserve(included.size());
    for (const Member& member : members) {
        if (!included.contains(member.PathKey)) {
            continue;
        }
        cancellation.ThrowIfCancelled();
        changes.push_back(VpkChange{member.Path, ReadBlob(member.Blob)});
    }

    const std::filesystem::path seed = state_->directory / "incoming" / (NewUuid() + ".vpk");
    struct SeedFile {
        std::filesystem::path Path;
        ~SeedFile() {
            std::error_code error;
            std::filesystem::remove(Path, error);
        }
    } seed_file{seed};
    WriteEmptyDirectoryVpk(seed);
    const VpkArchive empty = VpkArchive::Open(seed);
    ArchiveTransaction::Write(empty, seed, destination, changes, false, cancellation);
}

}  // namespace skeleton_repair
