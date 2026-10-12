#include "content_store.hpp"

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

void RejectGameArchive(const std::filesystem::path& absolute) {
    if (EqualsIgnoreCase(PathText(absolute.filename()), "pak01_dir.vpk")) {
        throw InvalidOperation(kGameArchiveMessage);
    }
    const std::filesystem::path parent = absolute.parent_path();
    if (!parent.empty() && std::filesystem::exists(parent / "pak01_dir.vpk")) {
        throw InvalidOperation(kGameFolderMessage);
    }
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
    RejectGameArchive(absolute);

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

}  // namespace skeleton_repair
