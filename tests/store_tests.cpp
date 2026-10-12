#include "content_hash.hpp"
#include "content_store.hpp"
#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"
#include "strings.hpp"
#include "updater.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using skeleton_repair::BlobId;
using skeleton_repair::CancellationSource;
using skeleton_repair::ContentStore;
using skeleton_repair::GcStats;
using skeleton_repair::HeroModelUpdater;
using skeleton_repair::History;
using skeleton_repair::HistoryVersion;
using skeleton_repair::SlotObservation;
using skeleton_repair::InvalidData;
using skeleton_repair::InvalidOperation;
using skeleton_repair::IoError;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::MembershipRow;
using skeleton_repair::ModelOutcome;
using skeleton_repair::ModelRow;
using skeleton_repair::Narrow;
using skeleton_repair::OperationCancelled;
using skeleton_repair::PackRow;
using skeleton_repair::RepairChange;
using skeleton_repair::RepairModelNote;
using skeleton_repair::RepairRequest;
using skeleton_repair::Sha256;
using skeleton_repair::Sha256File;
using skeleton_repair::SnapshotId;
using skeleton_repair::ToLowerHex;
using skeleton_repair::UpdateReport;
using skeleton_repair::WriteKv3;
using skeleton_repair::VpkArchive;
using skeleton_repair::VpkChange;

class TempDir {
public:
    TempDir() {
        static int sequence = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("dmu-" + std::to_string(ticks) + "-" + std::to_string(++sequence));
        fs::create_directories(dir_);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(dir_, error);
    }

    const fs::path& Path() const { return dir_; }

private:
    fs::path dir_;
};

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.good()) << path.string();
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(output.good()) << path.string();
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.good()) << path.string();
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

uint32_t LoadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

fs::path WriteSeed(const fs::path& path) {
    std::vector<uint8_t> bytes;
    AppendU32(bytes, 0x55AA1234u);
    AppendU32(bytes, 2u);
    AppendU32(bytes, 1u);
    AppendU32(bytes, 0u);
    bytes.resize(28, 0);
    bytes.push_back(0);
    WriteBytes(path, bytes);
    return path;
}

void ArchiveWith(const fs::path& path, const std::vector<VpkChange>& changes) {
    WriteSeed(path);
    VpkArchive::Open(path).Write(path, changes);
}

std::vector<uint8_t> Load(const char* name) {
    const auto path = fs::path(SR_SOURCE_DIR) / "tests" / "data" / name;
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path.string();
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::vector<uint8_t> Resource(const std::vector<uint8_t>& data) {
    const int start = 16 + 12;
    std::vector<uint8_t> file(static_cast<size_t>(start) + data.size());
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, 1);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, static_cast<uint32_t>(start - 20));
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    return file;
}

void AppendI32(std::vector<uint8_t>& bytes, int32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 4);
    WriteI32(bytes, offset, value);
}

void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
    AppendU32(bytes, static_cast<uint32_t>(value));
    AppendU32(bytes, static_cast<uint32_t>(value >> 32));
}

std::vector<uint8_t> RerlBlock(const std::vector<std::string>& names) {
    std::vector<uint8_t> block;
    const auto count = static_cast<uint32_t>(names.size());
    AppendU32(block, 8);
    AppendU32(block, count);
    std::string strings;
    std::vector<int32_t> offsets;
    size_t cursor = 8 + static_cast<size_t>(count) * 16;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t name_field = 16 + static_cast<size_t>(i) * 16;
        offsets.push_back(static_cast<int32_t>(cursor - name_field));
        strings += names[i];
        strings.push_back('\0');
        cursor += names[i].size() + 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        AppendU64(block, 0);
        AppendI32(block, offsets[i]);
        AppendI32(block, 0);
    }
    block.insert(block.end(), strings.begin(), strings.end());
    return block;
}

std::vector<uint8_t> ResourceWithRerl(const std::vector<uint8_t>& data, const std::vector<uint8_t>& rerl) {
    const size_t start = 40;
    std::vector<uint8_t> file(start + data.size() + rerl.size());
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, 2);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, 20);
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    file[28] = 'R';
    file[29] = 'E';
    file[30] = 'R';
    file[31] = 'L';
    WriteU32(file, 32, static_cast<uint32_t>(8 + data.size()));
    WriteI32(file, 36, static_cast<int32_t>(rerl.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    if (!rerl.empty()) {
        std::memcpy(file.data() + start + data.size(), rerl.data(), rerl.size());
    }
    return file;
}

int CountBlobFiles(const fs::path& store) {
    int count = 0;
    const fs::path root = store / "blobs" / "sha256";
    if (!fs::exists(root)) {
        return 0;
    }
    for (const fs::directory_entry& shard : fs::directory_iterator(root)) {
        if (!shard.is_directory()) {
            continue;
        }
        for (const fs::directory_entry& file : fs::directory_iterator(shard.path())) {
            if (file.is_regular_file()) {
                ++count;
            }
        }
    }
    return count;
}

bool IsLowerHex(std::string_view text, size_t size) {
    if (text.size() != size) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

BlobId IdOf(const std::vector<uint8_t>& bytes) {
    BlobId id;
    id.Bytes = Sha256(bytes);
    return id;
}

bool ContainsVersion(const std::vector<HistoryVersion>& versions, const BlobId& id) {
    return std::any_of(versions.begin(), versions.end(), [&](const HistoryVersion& version) {
        return version.Blob.Bytes == id.Bytes;
    });
}

bool ContainsSlot(const std::vector<SlotObservation>& slots, const BlobId& id) {
    return std::any_of(slots.begin(), slots.end(), [&](const SlotObservation& slot) { return slot.Blob.Bytes == id.Bytes; });
}

std::vector<uint8_t> AsBytes(const std::array<uint8_t, 32>& hash) {
    return std::vector<uint8_t>(hash.begin(), hash.end());
}

template <typename Fn>
void ExpectMessage(Fn&& call, const char* message) {
    try {
        std::forward<Fn>(call)();
        ADD_FAILURE() << "expected exception: " << message;
    } catch (const std::exception& error) {
        EXPECT_STREQ(error.what(), message);
    }
}

class Catalog {
public:
    explicit Catalog(const fs::path& store) {
        const fs::path db_path = store / "catalog.sqlite";
        if (sqlite3_open16(db_path.c_str(), &db_) != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "open failed";
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error(message);
        }
    }

    ~Catalog() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    int64_t Int(const char* sql) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        const int64_t value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }

    std::string Text(const char* sql) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        const auto* text = sqlite3_column_text(stmt, 0);
        const std::string value = text != nullptr ? reinterpret_cast<const char*>(text) : "";
        sqlite3_finalize(stmt);
        return value;
    }

    std::vector<std::string> Texts(const char* sql) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        std::vector<std::string> values;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(stmt, 0);
            values.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(stmt);
        return values;
    }

    std::vector<std::string> Column(const char* sql, int index) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        std::vector<std::string> values;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(stmt, index);
            values.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
        }
        sqlite3_finalize(stmt);
        return values;
    }

    std::vector<uint8_t> Blob(const char* sql) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
        const int size = sqlite3_column_bytes(stmt, 0);
        std::vector<uint8_t> value;
        if (data != nullptr && size > 0) {
            value.assign(data, data + size);
        }
        sqlite3_finalize(stmt);
        return value;
    }

    std::vector<uint8_t> BlobWhere(const char* sql, const std::array<uint8_t, 32>& key) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_bind_blob(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC), SQLITE_OK);
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        std::vector<uint8_t> value;
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
            const int size = sqlite3_column_bytes(stmt, 0);
            if (data != nullptr && size > 0) {
                value.assign(data, data + size);
            }
        }
        sqlite3_finalize(stmt);
        return value;
    }

    bool NullWhere(const char* sql, const std::array<uint8_t, 32>& key) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_bind_blob(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC), SQLITE_OK);
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        const bool is_null = sqlite3_column_type(stmt, 0) == SQLITE_NULL;
        sqlite3_finalize(stmt);
        return is_null;
    }

    int64_t IntWhere(const char* sql, const std::array<uint8_t, 32>& key) const {
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), SQLITE_OK) << sql;
        EXPECT_EQ(sqlite3_bind_blob(stmt, 1, key.data(), static_cast<int>(key.size()), SQLITE_STATIC), SQLITE_OK);
        EXPECT_EQ(sqlite3_step(stmt), SQLITE_ROW) << sql;
        const int64_t value = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }

    void Exec(const char* sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        if (rc != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "exec failed";
            sqlite3_free(error);
            ADD_FAILURE() << message;
        }
    }

private:
    sqlite3* db_ = nullptr;
};

const ModelRow* FindModel(const std::vector<ModelRow>& rows, const BlobId& id) {
    for (const ModelRow& row : rows) {
        if (row.Blob.Bytes == id.Bytes) {
            return &row;
        }
    }
    return nullptr;
}

bool HasBlob(const std::vector<ModelRow>& rows, const BlobId& id) { return FindModel(rows, id) != nullptr; }

std::string ManifestRow(const std::string& path, const std::vector<uint8_t>& payload, const char* kind) {
    return path + "\t" + ToLowerHex(Sha256(payload)) + "\t" + std::to_string(VpkArchive::Crc(payload)) + "\t" + kind + "\n";
}

std::string Utf8(const fs::path& path) { return Narrow(path.wstring()); }

KvValue Strings(const std::vector<std::string>& values) {
    std::vector<KvValue> items;
    for (const std::string& value : values) {
        items.push_back(KvValue::String(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Ints(const std::vector<int>& values) {
    std::vector<KvValue> items;
    for (int value : values) {
        items.push_back(KvValue::Int32(value));
    }
    return KvValue::Array(std::move(items));
}

struct HeroSpec {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    std::vector<int> Remap;
    float Pose = 0;
};

std::vector<uint8_t> HeroModel(const HeroSpec& spec) {
    std::vector<KvValue> positions;
    std::vector<KvValue> rotations;
    std::vector<KvValue> spheres;
    std::vector<KvValue> scales;
    for (size_t i = 0; i < spec.Names.size(); ++i) {
        positions.push_back(KvValue::Array({KvValue::Float(spec.Pose), KvValue::Float(0), KvValue::Float(0)}));
        rotations.push_back(KvValue::Array({KvValue::Float(0), KvValue::Float(0), KvValue::Float(0), KvValue::Float(1)}));
        spheres.push_back(KvValue::Float(1));
        scales.push_back(KvValue::Float(1));
    }
    KvValue skeleton = KvValue::Object({});
    skeleton.Set("m_boneName", Strings(spec.Names));
    skeleton.Set("m_nParent", Ints(spec.Parents));
    skeleton.Set("m_boneSphere", KvValue::Array(std::move(spheres)));
    skeleton.Set("m_nFlag", Ints(std::vector<int>(spec.Names.size(), 0)));
    skeleton.Set("m_bonePosParent", KvValue::Array(std::move(positions)));
    skeleton.Set("m_boneRotParent", KvValue::Array(std::move(rotations)));
    skeleton.Set("m_boneScaleParent", KvValue::Array(std::move(scales)));
    KvValue root = KvValue::Object({});
    root.Set("m_name", KvValue::String("models/heroes/test/test.vmdl"));
    root.Set("m_modelSkeleton", std::move(skeleton));
    root.Set("m_remappingTable", Ints(spec.Remap));
    KvDocument document;
    document.Root = std::move(root);
    return Resource(WriteKv3(document));
}

std::vector<uint8_t> EntryBytes(const VpkArchive& archive, std::string_view path) {
    for (const auto& entry : archive.Entries()) {
        if (entry.FullPath == path) {
            return archive.Read(entry);
        }
    }
    ADD_FAILURE() << path;
    return {};
}

}  // namespace

TEST(Store, Sha256MatchesKnownAnswers) {
    EXPECT_EQ(ToLowerHex(Sha256({})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char abc[] = "abc";
    EXPECT_EQ(ToLowerHex(Sha256(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(abc), 3))),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    TempDir dir;
    const fs::path file = dir.Path() / "bytes.bin";
    const std::vector<uint8_t> payload{1, 2, 3, 4, 5};
    WriteBytes(file, payload);
    EXPECT_EQ(Sha256File(file), Sha256(payload));
}

TEST(Store, SchemaVersionIsOne) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    {
        ContentStore store = ContentStore::Open(store_path);
    }
    ContentStore::Open(store_path);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Text("SELECT value FROM meta WHERE key = 'schema_version'"), "1");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM meta"), 1);
    EXPECT_EQ(catalog.Text("PRAGMA journal_mode"), "wal");
    EXPECT_EQ(catalog.Texts("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"),
        (std::vector<std::string>{"blobs", "edges", "lineages", "memberships", "meta", "observations", "refs", "snapshots", "transforms"}));
    EXPECT_EQ(catalog.Texts("SELECT name FROM sqlite_master WHERE type = 'index' AND name NOT LIKE 'sqlite_%' ORDER BY name"),
        (std::vector<std::string>{
            "idx_blobs_data",
            "idx_edges_input",
            "idx_edges_lineage",
            "idx_edges_output",
            "idx_memberships_blob",
            "idx_memberships_lineage",
        }));
    EXPECT_EQ(catalog.Column("PRAGMA table_info(blobs)", 1),
        (std::vector<std::string>{"sha256", "size", "codec", "stored_size", "data_sha256", "bytes_state", "created_utc"}));
    EXPECT_EQ(catalog.Column("PRAGMA table_info(memberships)", 1),
        (std::vector<std::string>{"snapshot_id", "path", "path_key", "blob_sha256", "crc32", "kind", "lineage_id"}));
    EXPECT_EQ(catalog.Column("PRAGMA table_info(observations)", 1),
        (std::vector<std::string>{"id", "snapshot_id", "absolute_path", "container_sha256", "file_size", "observed_utc", "source"}));
    EXPECT_EQ(catalog.Column("PRAGMA table_info(refs)", 1),
        (std::vector<std::string>{"snapshot_id", "from_path_key", "name", "resolved_path_key", "resolved_blob"}));
    EXPECT_EQ(catalog.Column("PRAGMA table_info(edges)", 1),
        (std::vector<std::string>{
            "id",
            "transform_id",
            "relation",
            "input_blob",
            "input_path_key",
            "input_data_sha256",
            "output_blob",
            "output_path_key",
            "output_data_sha256",
            "lineage_id",
            "detail_json",
        }));
}

TEST(Store, UnsupportedSchemaIsRejected) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    {
        ContentStore::Open(store_path);
    }
    {
        Catalog catalog(store_path);
        catalog.Exec("UPDATE meta SET value = '2' WHERE key = 'schema_version'");
    }
    ExpectMessage([&] { ContentStore::Open(store_path); }, "Unsupported model library schema.");
}

TEST(Store, TwoModelsAreDistinctBlobs) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "mod.vpk";
    const std::vector<uint8_t> model_a{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    const std::vector<uint8_t> model_b{9, 8, 7};
    ASSERT_EQ(VpkArchive::Crc(model_a), 0xCBF43926u);
    ArchiveWith(vpk, {
        VpkChange{"models/b.vmdl_c", model_b},
        VpkChange{"models/a.vmdl_c", model_a},
    });

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId snapshot = store.ImportVpk(vpk);
    EXPECT_TRUE(IsLowerHex(snapshot.Value, 32));
    EXPECT_EQ(store.ReadBlob(IdOf(model_a)), model_a);
    EXPECT_EQ(store.ReadBlob(IdOf(model_b)), model_b);
    EXPECT_EQ(CountBlobFiles(store_path), 2);
    EXPECT_TRUE(fs::is_empty(store_path / "incoming"));
    EXPECT_TRUE(fs::exists(store_path / "blobs" / "sha256" / IdOf(model_a).Hex().substr(0, 2) / IdOf(model_a).Hex()));

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM lineages"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM memberships"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    EXPECT_EQ(catalog.Text("SELECT codec FROM blobs LIMIT 1"), "none");
    EXPECT_EQ(catalog.Text("SELECT bytes_state FROM blobs LIMIT 1"), "present");
    EXPECT_EQ(catalog.Text("SELECT source FROM observations"), "import");
    EXPECT_EQ(catalog.IntWhere("SELECT crc32 FROM memberships WHERE blob_sha256 = ?", IdOf(model_a).Bytes),
        static_cast<int64_t>(0xCBF43926u));
    EXPECT_TRUE(catalog.NullWhere("SELECT data_sha256 FROM blobs WHERE sha256 = ?", IdOf(model_a).Bytes));
    EXPECT_TRUE(IsLowerHex(catalog.Text("SELECT id FROM lineages LIMIT 1"), 32));

    const std::string manifest = std::string("dmu-manifest-v1\n") + ManifestRow("models/a.vmdl_c", model_a, "vmdl_c")
        + ManifestRow("models/b.vmdl_c", model_b, "vmdl_c");
    EXPECT_EQ(catalog.Blob("SELECT manifest_sha256 FROM snapshots"), AsBytes(Sha256(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()))));
    EXPECT_EQ(catalog.Blob("SELECT container_sha256 FROM observations"), AsBytes(Sha256File(vpk)));
}

TEST(Store, SharedTextureIsOneBlob) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> texture{7, 7, 7, 7};
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const fs::path first = dir.Path() / "one.vpk";
    const fs::path second = dir.Path() / "two.vpk";
    ArchiveWith(first, {
        VpkChange{"models/a.vmdl_c", model_a},
        VpkChange{"materials/shared.vtex_c", texture},
    });
    ArchiveWith(second, {
        VpkChange{"models/b.vmdl_c", model_b},
        VpkChange{"materials/shared.vtex_c", texture},
    });

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(first);
    store.ImportVpk(second);

    EXPECT_EQ(CountBlobFiles(store_path), 3);
    EXPECT_EQ(store.ReadBlob(IdOf(texture)), texture);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM memberships WHERE blob_sha256 = ?", IdOf(texture).Bytes), 2);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM lineages WHERE root_sha256 = ?", IdOf(texture).Bytes), 1);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM blobs WHERE sha256 = ?", IdOf(texture).Bytes), 1);
    EXPECT_EQ(catalog.Text("SELECT kind FROM memberships WHERE path_key = 'materials/shared.vtex_c' LIMIT 1"), "vtex_c");
}

TEST(Store, ReimportUpdatesObservedUtc) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "mod.vpk";
    const std::vector<uint8_t> model{4, 5, 6};
    ArchiveWith(vpk, {VpkChange{"models/a.vmdl_c", model}});

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId first = store.ImportVpk(vpk);
    Catalog catalog(store_path);
    const std::string seen = catalog.Text("SELECT observed_utc FROM observations");
    const int blobs = CountBlobFiles(store_path);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const SnapshotId second = store.ImportVpk(vpk);
    std::string updated = catalog.Text("SELECT observed_utc FROM observations");
    if (updated == seen) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        store.ImportVpk(vpk);
        updated = catalog.Text("SELECT observed_utc FROM observations");
    }

    EXPECT_EQ(first.Value, second.Value);
    EXPECT_NE(updated, seen);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 1);
    EXPECT_EQ(CountBlobFiles(store_path), blobs);
}

TEST(Store, RepackSharesSnapshotAndAddsObservation) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model{8, 8, 8};
    const std::vector<uint8_t> note{1, 2, 3, 4};
    const std::vector<VpkChange> changes{
        VpkChange{"models/a.vmdl_c", model},
        VpkChange{"materials/readme.txt", note},
    };
    const fs::path original = dir.Path() / "original.vpk";
    const fs::path repack = dir.Path() / "repack.vpk";
    ArchiveWith(original, changes);
    ArchiveWith(repack, changes);

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId first = store.ImportVpk(original);
    const int blobs = CountBlobFiles(store_path);
    const SnapshotId second = store.ImportVpk(repack);

    EXPECT_EQ(first.Value, second.Value);
    EXPECT_EQ(CountBlobFiles(store_path), blobs);
    EXPECT_EQ(store.ReadBlob(IdOf(model)), model);
    EXPECT_EQ(store.ReadBlob(IdOf(note)), note);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), blobs);
    const std::vector<std::string> paths = catalog.Texts("SELECT absolute_path FROM observations ORDER BY absolute_path");
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(catalog.Blob("SELECT container_sha256 FROM observations ORDER BY absolute_path LIMIT 1"),
        AsBytes(Sha256File(fs::path(paths[0]))));
}

TEST(Store, Version12DataHashFollowsTheDataBlock) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> original_data = Load("empty-object.kv3");
    const std::vector<uint8_t> replacement_data = Load("scalars.kv3");
    const std::vector<uint8_t> original = Resource(original_data);
    const std::vector<uint8_t> patched = skeleton_repair::ReplaceDataBlock(original, replacement_data);
    const fs::path first = dir.Path() / "original.vpk";
    const fs::path second = dir.Path() / "patched.vpk";
    ArchiveWith(first, {VpkChange{"models/hero.vmdl_c", original}});
    ArchiveWith(second, {VpkChange{"models/hero.vmdl_c", patched}});

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(first);
    store.ImportVpk(second);

    Catalog catalog(store_path);
    const std::vector<uint8_t> original_hash = AsBytes(Sha256(original_data));
    const std::vector<uint8_t> patched_hash = AsBytes(Sha256(replacement_data));
    EXPECT_EQ(catalog.BlobWhere("SELECT data_sha256 FROM blobs WHERE sha256 = ?", IdOf(original).Bytes), original_hash);
    EXPECT_EQ(catalog.BlobWhere("SELECT data_sha256 FROM blobs WHERE sha256 = ?", IdOf(patched).Bytes), patched_hash);
    EXPECT_NE(IdOf(original).Hex(), IdOf(patched).Hex());
    EXPECT_NE(original_hash, patched_hash);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 2);
}

TEST(Store, NonResourceHasNullDataHash) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "notes.vpk";
    const std::vector<uint8_t> note{1, 2, 3, 4};
    ArchiveWith(vpk, {VpkChange{"materials/readme.txt", note}});

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(vpk);
    EXPECT_EQ(store.ReadBlob(IdOf(note)), note);

    Catalog catalog(store_path);
    EXPECT_TRUE(catalog.NullWhere("SELECT data_sha256 FROM blobs WHERE sha256 = ?", IdOf(note).Bytes));
    EXPECT_EQ(catalog.Text("SELECT kind FROM memberships"), "other");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
}

TEST(Store, ReadBlobReturnsThePayload) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "mod.vpk";
    const std::vector<uint8_t> payload{9, 4, 2, 0, 255, 7};
    ArchiveWith(vpk, {VpkChange{"models/hero.vmdl_c", payload}});
    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(vpk);
    EXPECT_EQ(store.ReadBlob(IdOf(payload)), payload);

    BlobId missing;
    missing.Bytes.fill(0xAB);
    EXPECT_THROW(store.ReadBlob(missing), InvalidData);
}

TEST(Store, CrcDamageCommitsNothing) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "corrupt.vpk";
    ArchiveWith(vpk, {VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1, 2, 3}}});
    auto bytes = ReadBytes(vpk);
    bytes[28 + LoadU32(bytes, 8)] ^= 0xFF;
    WriteBytes(vpk, bytes);

    ContentStore store = ContentStore::Open(store_path);
    EXPECT_THROW(store.ImportVpk(vpk), InvalidData);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 0);
    EXPECT_EQ(CountBlobFiles(store_path), 0);
}

TEST(Store, CancelBeforeCommitCommitsNothing) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1, 2, 3, 4}}});
    ContentStore store = ContentStore::Open(store_path);
    CancellationSource source;
    source.Cancel();
    EXPECT_THROW(store.ImportVpk(vpk, source.Token()), OperationCancelled);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 0);
    EXPECT_EQ(CountBlobFiles(store_path), 0);
}

TEST(Store, ChunkFileIsRejected) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path chunk = dir.Path() / "pack_000.vpk";
    const fs::path upper = dir.Path() / "Mod_007.VPK";
    const fs::path kept = dir.Path() / "notes_12.vpk";
    const std::vector<uint8_t> payload{1, 2, 3};
    ArchiveWith(chunk, {VpkChange{"models/a.vmdl_c", payload}});
    ArchiveWith(upper, {VpkChange{"models/a.vmdl_c", payload}});
    ArchiveWith(kept, {VpkChange{"models/a.vmdl_c", payload}});

    ContentStore store = ContentStore::Open(store_path);
    ExpectMessage([&] { store.ImportVpk(chunk); }, "Import the directory VPK, not a chunk file.");
    ExpectMessage([&] { store.ImportVpk(upper); }, "Import the directory VPK, not a chunk file.");
    EXPECT_EQ(CountBlobFiles(store_path), 0);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);

    const SnapshotId imported = store.ImportVpk(kept);
    EXPECT_TRUE(IsLowerHex(imported.Value, 32));
    EXPECT_EQ(CountBlobFiles(store_path), 1);
    EXPECT_EQ(store.ReadBlob(IdOf(payload)), payload);
}

TEST(Store, MissingAndNonVpkImportNothing) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    ContentStore store = ContentStore::Open(store_path);
    ExpectMessage([&] { store.ImportVpk(dir.Path() / "absent.vpk"); }, "Could not find the mod VPK.");
    const fs::path notes = dir.Path() / "notes.vpk";
    WriteBytes(notes, {1, 2, 3, 4});
    EXPECT_THROW(store.ImportVpk(notes), InvalidData);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_EQ(CountBlobFiles(store_path), 0);
}

TEST(Store, GameArchiveIsNotImported) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path game = dir.Path() / "citadel";
    fs::create_directories(game);
    const fs::path pak = game / "pak01_dir.vpk";
    const fs::path mod = game / "mod.vpk";
    const std::vector<uint8_t> payload{5, 5, 5, 5};
    ArchiveWith(pak, {VpkChange{"models/heroes/hero.vmdl_c", payload}});
    ArchiveWith(mod, {VpkChange{"models/heroes/hero.vmdl_c", payload}});

    ContentStore store = ContentStore::Open(store_path);
    ExpectMessage([&] { store.ImportVpk(pak); }, "That file is the game archive. Choose a mod VPK from the addons folder.");
    ExpectMessage([&] { store.ImportVpk(mod); }, "Choose a mod outside the game's archive folder, such as a VPK in addons.");
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 0);
    EXPECT_EQ(CountBlobFiles(store_path), 0);
}

TEST(Store, BusyLibraryIsReportedAndRetryUsesExistingBlobFile) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "mod.vpk";
    const std::vector<uint8_t> payload{6, 6, 6};
    ArchiveWith(vpk, {VpkChange{"models/a.vmdl_c", payload}});
    ContentStore store = ContentStore::Open(store_path);

    sqlite3* lock = nullptr;
    ASSERT_EQ(sqlite3_open16((store_path / "catalog.sqlite").c_str(), &lock), SQLITE_OK);
    sqlite3_busy_timeout(lock, 0);
    ASSERT_EQ(sqlite3_exec(lock, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
    ExpectMessage([&] { store.ImportVpk(vpk); }, "The model library is in use by another process.");
    EXPECT_EQ(sqlite3_exec(lock, "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(lock);

    Catalog during(store_path);
    EXPECT_EQ(during.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_GT(CountBlobFiles(store_path), 0);

    const SnapshotId snapshot = store.ImportVpk(vpk);
    EXPECT_TRUE(IsLowerHex(snapshot.Value, 32));
    EXPECT_EQ(store.ReadBlob(IdOf(payload)), payload);
    EXPECT_EQ(during.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(during.Int("SELECT COUNT(*) FROM blobs"), 1);
    EXPECT_EQ(CountBlobFiles(store_path), 1);
}

TEST(Store, ReferencesResolveInsideTheSnapshot) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    KvDocument document;
    document.Root.Set("m_vecNmSkeletonRefs", KvValue::Array({
        KvValue::String("models/heroes/Test/test.vnmskel"),
        KvValue::String("models/missing.vnmskel"),
    }));
    const std::vector<uint8_t> model = ResourceWithRerl(skeleton_repair::WriteKv3(document), RerlBlock({"materials/shared.vtex"}));
    const std::vector<uint8_t> texture{3, 3, 3};
    const std::vector<uint8_t> skeleton{4, 4};
    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {
        VpkChange{"models/heroes/Test/hero.vmdl_c", model},
        VpkChange{"materials/shared.vtex_c", texture},
        VpkChange{"models/heroes/Test/test.vnmskel_c", skeleton},
    });

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(vpk);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Texts("SELECT name FROM refs ORDER BY name"),
        (std::vector<std::string>{
            "materials/shared.vtex",
            "models/heroes/Test/test.vnmskel_c",
            "models/missing.vnmskel_c",
        }));
    EXPECT_EQ(catalog.Texts("SELECT IFNULL(resolved_path_key, '') FROM refs ORDER BY name"),
        (std::vector<std::string>{
            "materials/shared.vtex_c",
            "models/heroes/test/test.vnmskel_c",
            "",
        }));
    EXPECT_EQ(catalog.Text("SELECT path FROM memberships WHERE path_key = 'models/heroes/test/test.vnmskel_c'"),
        "models/heroes/Test/test.vnmskel_c");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM refs WHERE resolved_blob IS NULL"), 1);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM refs WHERE resolved_blob = ?", IdOf(texture).Bytes), 1);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM refs WHERE resolved_blob = ?", IdOf(skeleton).Bytes), 1);
}

TEST(Store, BrokenReferenceListStillImports) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    auto broken = ResourceWithRerl(Load("empty-object.kv3"), RerlBlock({"models/heroes/test/missing.vnmskel"}));
    WriteI32(broken, 36, 4);
    const fs::path vpk = dir.Path() / "broken.vpk";
    ArchiveWith(vpk, {VpkChange{"models/broken.vmdl_c", broken}});

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId snapshot = store.ImportVpk(vpk);
    EXPECT_TRUE(IsLowerHex(snapshot.Value, 32));
    EXPECT_EQ(store.ReadBlob(IdOf(broken)), broken);
    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM refs"), 0);
    EXPECT_FALSE(catalog.NullWhere("SELECT data_sha256 FROM blobs WHERE sha256 = ?", IdOf(broken).Bytes));
}

TEST(Store, LibraryListsOneRowPerModelPayload) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const std::vector<uint8_t> model_c{3, 3, 3};
    const std::vector<uint8_t> texture{7, 7, 7, 7};
    const fs::path alpha = dir.Path() / "alpha.vpk";
    const fs::path beta = dir.Path() / "beta.vpk";
    ArchiveWith(alpha, {
        VpkChange{"models/b.vmdl_c", model_b},
        VpkChange{"models/a.vmdl_c", model_a},
        VpkChange{"materials/shared.vtex_c", texture},
    });
    ArchiveWith(beta, {
        VpkChange{"models/heroes/shared/a.vmdl_c", model_a},
        VpkChange{"models/c.vmdl_c", model_c},
        VpkChange{"materials/shared.vtex_c", texture},
    });

    ContentStore store = ContentStore::Open(store_path);
    EXPECT_TRUE(store.ListModels().empty());
    EXPECT_TRUE(store.ListPacks().empty());
    EXPECT_TRUE(store.Memberships(IdOf(model_a)).empty());
    EXPECT_TRUE(store.ExactDuplicateModels().empty());

    store.ImportVpk(alpha);
    const std::vector<ModelRow> first_models = store.ListModels();
    const std::vector<PackRow> first_packs = store.ListPacks();
    ASSERT_EQ(first_models.size(), 2u);
    EXPECT_EQ(first_models[0].DisplayPath, "models/a.vmdl_c");
    EXPECT_EQ(first_models[1].DisplayPath, "models/b.vmdl_c");
    EXPECT_EQ(first_models[0].Blob.Bytes, IdOf(model_a).Bytes);
    EXPECT_EQ(first_models[1].Blob.Bytes, IdOf(model_b).Bytes);
    EXPECT_FALSE(HasBlob(first_models, IdOf(texture)));
    ASSERT_EQ(first_packs.size(), 1u);
    EXPECT_EQ(first_packs[0].Label, "alpha.vpk");
    EXPECT_EQ(first_packs[0].ModelCount, 2);
    EXPECT_EQ(first_packs[0].ObservationCount, 1);
    EXPECT_FALSE(first_packs[0].ObservedUtc.empty());
    EXPECT_TRUE(store.ExactDuplicateModels().empty());

    store.ImportVpk(beta);
    const std::vector<ModelRow> models = store.ListModels();
    ASSERT_EQ(models.size(), 3u);
    EXPECT_FALSE(HasBlob(models, IdOf(texture)));
    const ModelRow* shared = FindModel(models, IdOf(model_a));
    const ModelRow* once_b = FindModel(models, IdOf(model_b));
    const ModelRow* once_c = FindModel(models, IdOf(model_c));
    ASSERT_NE(shared, nullptr);
    ASSERT_NE(once_b, nullptr);
    ASSERT_NE(once_c, nullptr);
    EXPECT_EQ(shared->DisplayPath, "models/a.vmdl_c");
    EXPECT_EQ(shared->SnapshotCount, 2);
    EXPECT_EQ(shared->PathCount, 2);
    EXPECT_TRUE(IsLowerHex(shared->LineageId, 32));
    EXPECT_EQ(once_b->SnapshotCount, 1);
    EXPECT_EQ(once_b->PathCount, 1);
    EXPECT_EQ(once_c->SnapshotCount, 1);
    EXPECT_EQ(once_c->PathCount, 1);

    const std::vector<ModelRow> duplicates = store.ExactDuplicateModels();
    ASSERT_EQ(duplicates.size(), 1u);
    EXPECT_EQ(duplicates[0].Blob.Bytes, IdOf(model_a).Bytes);
    EXPECT_FALSE(HasBlob(duplicates, IdOf(model_b)));
    EXPECT_FALSE(HasBlob(duplicates, IdOf(model_c)));

    const std::vector<MembershipRow> memberships = store.Memberships(IdOf(model_a));
    ASSERT_EQ(memberships.size(), 2u);
    EXPECT_EQ(memberships[0].Path, "models/a.vmdl_c");
    EXPECT_EQ(memberships[0].PackLabel, "alpha.vpk");
    EXPECT_EQ(memberships[1].Path, "models/heroes/shared/a.vmdl_c");
    EXPECT_EQ(memberships[1].PackLabel, "beta.vpk");
    EXPECT_FALSE(memberships[0].ObservedUtc.empty());
    EXPECT_FALSE(memberships[1].ObservedUtc.empty());

    const std::vector<MembershipRow> texture_rows = store.Memberships(IdOf(texture));
    ASSERT_EQ(texture_rows.size(), 2u);
    EXPECT_EQ(texture_rows[0].Path, "materials/shared.vtex_c");
    EXPECT_EQ(texture_rows[1].Path, "materials/shared.vtex_c");

    const std::vector<PackRow> packs = store.ListPacks();
    ASSERT_EQ(packs.size(), 2u);
    EXPECT_EQ(packs[0].ModelCount, 2);
    EXPECT_EQ(packs[1].ModelCount, 2);
}

TEST(Store, TwoPathsOfOnePayloadAreOneDuplicateRow) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> shared{4, 4, 4};
    const std::vector<uint8_t> once{5, 5};
    const fs::path vpk = dir.Path() / "paths.vpk";
    ArchiveWith(vpk, {
        VpkChange{"models/nested/longer/shared.vmdl_c", shared},
        VpkChange{"models/z.vmdl_c", shared},
        VpkChange{"models/once.vmdl_c", once},
    });

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(vpk);
    const std::vector<ModelRow> models = store.ListModels();
    ASSERT_EQ(models.size(), 2u);
    const ModelRow* duplicate = FindModel(models, IdOf(shared));
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->DisplayPath, "models/z.vmdl_c");
    EXPECT_EQ(duplicate->SnapshotCount, 1);
    EXPECT_EQ(duplicate->PathCount, 2);

    const std::vector<ModelRow> duplicates = store.ExactDuplicateModels();
    ASSERT_EQ(duplicates.size(), 1u);
    EXPECT_EQ(duplicates[0].Blob.Bytes, IdOf(shared).Bytes);
    EXPECT_FALSE(HasBlob(duplicates, IdOf(once)));

    const std::vector<PackRow> packs = store.ListPacks();
    ASSERT_EQ(packs.size(), 1u);
    EXPECT_EQ(packs[0].ModelCount, 3);
    EXPECT_EQ(packs[0].Label, "paths.vpk");
}

TEST(Store, TwoObservationsAreOnePack) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model{8, 8, 8};
    const fs::path original = dir.Path() / "original.vpk";
    const fs::path repack = dir.Path() / "repack.vpk";
    const std::vector<VpkChange> changes{VpkChange{"models/a.vmdl_c", model}};
    ArchiveWith(original, changes);
    ArchiveWith(repack, changes);

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(original);
    const std::string first_seen = store.ListPacks().at(0).ObservedUtc;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    store.ImportVpk(repack);
    std::vector<PackRow> packs = store.ListPacks();
    ASSERT_EQ(packs.size(), 1u);
    if (packs[0].ObservedUtc == first_seen) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        store.ImportVpk(repack);
        packs = store.ListPacks();
    }

    ASSERT_EQ(packs.size(), 1u);
    EXPECT_EQ(packs[0].ObservationCount, 2);
    EXPECT_EQ(packs[0].ModelCount, 1);
    EXPECT_EQ(packs[0].Label, "repack.vpk");
    EXPECT_GT(packs[0].ObservedUtc, first_seen);
    EXPECT_EQ(store.ListModels().size(), 1u);
    ASSERT_EQ(store.Memberships(IdOf(model)).size(), 1u);
    EXPECT_EQ(store.Memberships(IdOf(model))[0].PackLabel, "repack.vpk");
    EXPECT_EQ(store.Memberships(IdOf(model))[0].ObservedUtc, packs[0].ObservedUtc);
}

TEST(Store, RepairRewritesOneModelAndReusesTheOther) {
    const std::vector<uint8_t> game_hero = HeroModel({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = {0, 1}});
    const std::vector<uint8_t> old_hero = HeroModel({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = {0, 1}});
    const std::vector<uint8_t> side = HeroModel({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = {0, 1}, .Pose = 4.f});
    constexpr const char* kHero = "models/heroes/test/test.vmdl_c";
    constexpr const char* kSide = "models/heroes/side/side.vmdl_c";

    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path game_dir = dir.Path() / "game";
    const fs::path mod = dir.Path() / "mod.vpk";
    const fs::path output = dir.Path() / "repaired.vpk";
    fs::create_directories(game_dir);
    ArchiveWith(game_dir / "pak01_dir.vpk", {{kHero, game_hero}, {kSide, side}});
    ArchiveWith(mod, {{kHero, old_hero}, {kSide, side}});
    const auto original = ReadBytes(mod);

    ContentStore store = ContentStore::Open(store_path);
    const UpdateReport report = HeroModelUpdater::Update(Utf8(game_dir), Utf8(mod), Utf8(output), false, false, {}, {}, false, &store);
    EXPECT_TRUE(report.WroteFile);
    EXPECT_EQ(ReadBytes(mod), original);
    ASSERT_EQ(report.Results.size(), 2u);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM transforms"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges"), 1);
    EXPECT_EQ(catalog.Text("SELECT relation FROM edges"), "mutated");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges WHERE relation = 'copied'"), 0);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM blobs WHERE sha256 = ?", IdOf(game_hero).Bytes), 0);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges").find("Mesh bone indices were remapped."), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges").find(ToLowerHex(IdOf(game_hero).Bytes)), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT summary_json FROM transforms").find("repaired.vpk"), std::string::npos);

    const std::string hero_lineage = catalog.Text(
        "SELECT lineage_id FROM memberships WHERE path_key = 'models/heroes/test/test.vmdl_c' LIMIT 1");
    EXPECT_EQ(catalog.Text("SELECT lineage_id FROM edges"), hero_lineage);
    EXPECT_EQ(catalog.Int("SELECT COUNT(DISTINCT lineage_id) FROM memberships WHERE path_key = 'models/heroes/test/test.vmdl_c'"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(DISTINCT blob_sha256) FROM memberships WHERE path_key = 'models/heroes/test/test.vmdl_c'"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(DISTINCT blob_sha256) FROM memberships WHERE path_key = 'models/heroes/side/side.vmdl_c'"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(DISTINCT lineage_id) FROM memberships WHERE path_key = 'models/heroes/side/side.vmdl_c'"), 1);
    EXPECT_EQ(catalog.Int(
        "SELECT COUNT(*) FROM edges WHERE input_path_key = 'models/heroes/side/side.vmdl_c' "
        "OR output_path_key = 'models/heroes/side/side.vmdl_c'"), 0);

    const std::vector<ModelRow> models = store.ListModels();
    const ModelRow* side_row = FindModel(models, IdOf(side));
    const ModelRow* old_row = FindModel(models, IdOf(old_hero));
    ASSERT_NE(side_row, nullptr);
    ASSERT_NE(old_row, nullptr);
    EXPECT_EQ(side_row->SnapshotCount, 2);
    EXPECT_NE(side_row->LineageId, hero_lineage);
    EXPECT_EQ(old_row->LineageId, hero_lineage);
    EXPECT_EQ(old_row->SnapshotCount, 1);

    BlobId repaired_id{};
    for (const ModelRow& row : models) {
        if (row.DisplayPath == kHero && row.Blob.Bytes != IdOf(old_hero).Bytes) {
            repaired_id = row.Blob;
        }
    }
    EXPECT_EQ(repaired_id.Hex().size(), 64u);
    EXPECT_NE(repaired_id.Bytes, IdOf(old_hero).Bytes);
    const std::vector<uint8_t> written = EntryBytes(VpkArchive::Open(output), kHero);
    EXPECT_EQ(store.ReadBlob(repaired_id), written);
    EXPECT_NE(written, old_hero);
    EXPECT_NE(written, game_hero);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), 3);
    EXPECT_EQ(CountBlobFiles(store_path), 3);

    const int64_t snapshots = catalog.Int("SELECT COUNT(*) FROM snapshots");
    const int64_t blobs = catalog.Int("SELECT COUNT(*) FROM blobs");
    store.ImportVpk(output);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), snapshots);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), blobs);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 2);
    EXPECT_EQ(static_cast<int64_t>(CountBlobFiles(store_path)), blobs);
}

TEST(Store, BlockedRepairCommitsNoOutputSnapshot) {
    const std::vector<uint8_t> game_hero = HeroModel({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = {0, 1}});
    const std::vector<uint8_t> old_hero = HeroModel({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = {0, 1}});
    constexpr const char* kHero = "models/heroes/test/test.vmdl_c";
    constexpr const char* kBroken = "models/heroes/test/broken.vmdl_c";

    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path game_dir = dir.Path() / "game";
    const fs::path mod = dir.Path() / "mod.vpk";
    fs::create_directories(game_dir);
    ArchiveWith(game_dir / "pak01_dir.vpk", {{kHero, game_hero}, {kBroken, game_hero}});
    ArchiveWith(mod, {{kHero, old_hero}, {kBroken, std::vector<uint8_t>{1, 2, 3}}});
    const auto before = ReadBytes(mod);

    ContentStore store = ContentStore::Open(store_path);
    const UpdateReport report = HeroModelUpdater::Update(Utf8(game_dir), Utf8(mod), std::nullopt, true, true, {}, {}, false, &store);
    EXPECT_TRUE(report.HasFailures());
    EXPECT_FALSE(report.WroteFile);
    EXPECT_EQ(ReadBytes(mod), before);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM transforms"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges"), 0);
    EXPECT_EQ(CountBlobFiles(store_path), 0);
}

TEST(Store, RepairEdgesCoverRenameRemovalAndMutation) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const std::vector<uint8_t> model_c{3, 3, 3};
    const std::vector<uint8_t> model_d{4, 4, 4};
    const std::vector<uint8_t> texture{7, 7, 7, 7};
    const std::vector<uint8_t> model_a2{1, 1, 2};
    const std::vector<uint8_t> model_d2{4, 4, 5};
    const std::vector<uint8_t> game_only{9, 9, 9};
    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {
        VpkChange{"models/a.vmdl_c", model_a},
        VpkChange{"models/b.vmdl_c", model_b},
        VpkChange{"models/c.vmdl_c", model_c},
        VpkChange{"models/d.vmdl_c", model_d},
        VpkChange{"materials/t.vtex_c", texture},
    });

    ContentStore store = ContentStore::Open(store_path);
    ExpectMessage([&] {
        RepairRequest missing;
        missing.Input.Value = "0123456789abcdef0123456789abcdef";
        store.RecordRepair(missing);
    }, "The model library has no snapshot with that id.");
    EXPECT_EQ(CountBlobFiles(store_path), 0);

    const SnapshotId input = store.ImportVpk(vpk);
    const int blobs_before = CountBlobFiles(store_path);
    RepairRequest repair;
    repair.Input = input;
    repair.OutputPath = "D:/out/repaired.vpk";
    repair.BackupPath = "D:/out/repaired.vpk.bak";
    repair.Changes.push_back(RepairChange{"models/a.vmdl_c", "models/a.vmdl_c", model_a2});
    repair.Changes.push_back(RepairChange{"models/b.vmdl_c", "models/renamed/b.vmdl_c", model_b});
    repair.Changes.push_back(RepairChange{"models/c.vmdl_c", "", std::nullopt});
    repair.Changes.push_back(RepairChange{"models/d.vmdl_c", "models/moved/d.vmdl_c", model_d2});
    repair.Models.push_back(RepairModelNote{"models/a.vmdl_c", "Updated", "mutate-a", {}, {}, "models/heroes/test/test.vmdl_c", IdOf(game_only).Bytes});
    repair.Models.push_back(RepairModelNote{"models/b.vmdl_c", "Updated", "rename-b", {}, {}, {}, std::nullopt});
    repair.Models.push_back(RepairModelNote{"models/d.vmdl_c", "Updated", "move-d", {}, {}, {}, std::nullopt});

    const SnapshotId output = store.RecordRepair(repair);
    EXPECT_NE(output.Value, input.Value);
    EXPECT_EQ(CountBlobFiles(store_path), blobs_before + 2);
    EXPECT_EQ(store.ReadBlob(IdOf(model_a2)), model_a2);
    EXPECT_EQ(store.ReadBlob(IdOf(model_b)), model_b);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM lineages"), 5);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM blobs WHERE sha256 = ?", IdOf(game_only).Bytes), 0);
    EXPECT_EQ(catalog.Texts("SELECT relation FROM edges ORDER BY relation"),
        (std::vector<std::string>{"mutated", "renamed", "renamed+mutated"}));
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges WHERE relation = 'copied'"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges WHERE relation = 'renamed' AND input_blob = output_blob"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges WHERE relation = 'renamed+mutated' AND input_blob = output_blob"), 0);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges WHERE relation = 'mutated'").find("mutate-a"), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges WHERE relation = 'renamed'").find("rename-b"), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges WHERE relation = 'renamed+mutated'").find("move-d"), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT summary_json FROM transforms").find("repaired.vpk.bak"), std::string::npos);
    EXPECT_NE(catalog.Text("SELECT detail_json FROM edges WHERE relation = 'mutated'").find(ToLowerHex(IdOf(game_only).Bytes)), std::string::npos);

    const std::string output_filter = "snapshot_id = '" + output.Value + "'";
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM memberships WHERE " + output_filter + " AND path_key = 'models/c.vmdl_c'").c_str()), 0);
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM memberships WHERE " + output_filter + " AND path_key = 'models/b.vmdl_c'").c_str()), 0);
    EXPECT_EQ(catalog.Text(("SELECT path FROM memberships WHERE " + output_filter + " AND path_key = 'models/renamed/b.vmdl_c'").c_str()),
        "models/renamed/b.vmdl_c");
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM memberships WHERE " + output_filter).c_str()), 4);
    EXPECT_EQ(catalog.Text("SELECT lineage_id FROM edges WHERE relation = 'mutated'"),
        catalog.Text("SELECT lineage_id FROM memberships WHERE path_key = 'models/a.vmdl_c' LIMIT 1"));
    EXPECT_EQ(catalog.Text("SELECT lineage_id FROM edges WHERE relation = 'renamed'"),
        catalog.Text("SELECT lineage_id FROM memberships WHERE path_key = 'models/renamed/b.vmdl_c'"));
    EXPECT_EQ(catalog.Text("SELECT lineage_id FROM edges WHERE relation = 'renamed+mutated'"),
        catalog.Text("SELECT lineage_id FROM memberships WHERE path_key = 'models/d.vmdl_c' LIMIT 1"));
    EXPECT_EQ(catalog.Text(("SELECT lineage_id FROM memberships WHERE " + output_filter + " AND path_key = 'models/moved/d.vmdl_c'").c_str()),
        catalog.Text("SELECT lineage_id FROM edges WHERE relation = 'renamed+mutated'"));
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM lineages WHERE root_sha256 = ?", IdOf(model_d2).Bytes), 0);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM lineages WHERE root_sha256 = ?", IdOf(model_a2).Bytes), 0);
}

TEST(Store, HistoryOfMutatedBlobKeepsUntouchedAndUnprovenApart) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const std::vector<uint8_t> model_a2{1, 1, 2};
    const std::vector<uint8_t> other_a{9, 9, 9};
    const BlobId id_a = IdOf(model_a);
    const BlobId id_b = IdOf(model_b);
    const BlobId id_a2 = IdOf(model_a2);
    const BlobId id_other = IdOf(other_a);

    ContentStore store = ContentStore::Open(store_path);
    BlobId unknown{};
    unknown.Bytes.fill(0xab);
    EXPECT_TRUE(store.HistoryOf(unknown).LineageId.empty());
    EXPECT_TRUE(store.HistoryOf(unknown).Versions.empty());
    EXPECT_TRUE(store.HistoryOf(unknown).Links.empty());
    EXPECT_TRUE(store.UnprovenSamePath(unknown).empty());

    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {VpkChange{"models/a.vmdl_c", model_a}, VpkChange{"models/b.vmdl_c", model_b}});
    const SnapshotId input = store.ImportVpk(vpk);
    RepairRequest repair;
    repair.Input = input;
    repair.OutputPath = "D:/out/repaired.vpk";
    repair.Changes.push_back(RepairChange{"models/a.vmdl_c", "models/a.vmdl_c", model_a2});
    repair.Models.push_back(RepairModelNote{"models/a.vmdl_c", "Updated", "mutate-a", {}, {}, {}, std::nullopt});
    store.RecordRepair(repair);

    const fs::path other = dir.Path() / "other.vpk";
    ArchiveWith(other, {VpkChange{"models/a.vmdl_c", other_a}});
    store.ImportVpk(other);

    const History mutated = store.HistoryOf(id_a2);
    const History original = store.HistoryOf(id_a);
    ASSERT_EQ(mutated.Versions.size(), 2u);
    EXPECT_EQ(original.LineageId, mutated.LineageId);
    EXPECT_FALSE(mutated.LineageId.empty());
    EXPECT_EQ(mutated.Versions[0].Blob.Bytes, id_a.Bytes);
    EXPECT_EQ(mutated.Versions[1].Blob.Bytes, id_a2.Bytes);
    EXPECT_EQ(original.Versions[0].Blob.Bytes, id_a.Bytes);
    EXPECT_EQ(original.Versions[1].Blob.Bytes, id_a2.Bytes);
    EXPECT_FALSE(ContainsVersion(mutated.Versions, id_other));
    EXPECT_FALSE(ContainsVersion(mutated.Versions, id_b));
    EXPECT_FALSE(mutated.Versions[0].Data.has_value());
    EXPECT_FALSE(mutated.Versions[1].Data.has_value());
    ASSERT_EQ(mutated.Links.size(), 1u);
    EXPECT_EQ(mutated.Links[0].Relation, "mutated");
    EXPECT_EQ(mutated.Links[0].Detail, "mutate-a");
    EXPECT_EQ(mutated.Links[0].From.Bytes, id_a.Bytes);
    EXPECT_EQ(mutated.Links[0].To.Bytes, id_a2.Bytes);
    EXPECT_EQ(mutated.Links[0].ToolVersion, "1");
    EXPECT_FALSE(mutated.Links[0].FinishedUtc.empty());

    const History untouched = store.HistoryOf(id_b);
    ASSERT_EQ(untouched.Versions.size(), 1u);
    EXPECT_TRUE(untouched.Links.empty());
    EXPECT_EQ(untouched.Versions[0].Blob.Bytes, id_b.Bytes);
    EXPECT_GE(untouched.Versions[0].SnapshotCount, 2);
    EXPECT_NE(untouched.LineageId, mutated.LineageId);

    const std::vector<SlotObservation> from_original = store.UnprovenSamePath(id_a);
    const std::vector<SlotObservation> from_mutated = store.UnprovenSamePath(id_a2);
    ASSERT_EQ(from_original.size(), 1u);
    ASSERT_EQ(from_mutated.size(), 1u);
    EXPECT_EQ(from_original[0].Blob.Bytes, id_other.Bytes);
    EXPECT_EQ(from_mutated[0].Blob.Bytes, id_other.Bytes);
    EXPECT_EQ(from_original[0].Path, "models/a.vmdl_c");
    EXPECT_EQ(from_original[0].PackLabel, "other.vpk");
    EXPECT_FALSE(from_original[0].ObservedUtc.empty());
    EXPECT_NE(from_original[0].LineageId, mutated.LineageId);
    EXPECT_FALSE(ContainsSlot(from_original, id_a));
    EXPECT_FALSE(ContainsSlot(from_original, id_a2));
    EXPECT_FALSE(ContainsSlot(from_original, id_b));
    EXPECT_FALSE(ContainsSlot(from_mutated, id_a));
    EXPECT_FALSE(ContainsSlot(from_mutated, id_a2));
    EXPECT_FALSE(ContainsSlot(from_mutated, id_b));
    EXPECT_TRUE(store.UnprovenSamePath(id_b).empty());
}

TEST(Store, RenameOnlyKeepsOneVersionWithBothPaths) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model{5, 5, 5};
    const BlobId id = IdOf(model);
    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {VpkChange{"models/old.vmdl_c", model}});

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId input = store.ImportVpk(vpk);
    RepairRequest repair;
    repair.Input = input;
    repair.OutputPath = "D:/out/renamed.vpk";
    repair.Changes.push_back(RepairChange{"models/old.vmdl_c", "models/new/renamed.vmdl_c", model});
    repair.Models.push_back(RepairModelNote{"models/old.vmdl_c", "Updated", "rename-only", {}, {}, {}, std::nullopt});
    store.RecordRepair(repair);

    const History history = store.HistoryOf(id);
    ASSERT_EQ(history.Versions.size(), 1u);
    EXPECT_EQ(history.Versions[0].Blob.Bytes, id.Bytes);
    EXPECT_GE(history.Versions[0].SnapshotCount, 2);
    ASSERT_EQ(history.Versions[0].Paths.size(), 2u);
    EXPECT_NE(std::find(history.Versions[0].Paths.begin(), history.Versions[0].Paths.end(), "models/old.vmdl_c"),
        history.Versions[0].Paths.end());
    EXPECT_NE(std::find(history.Versions[0].Paths.begin(), history.Versions[0].Paths.end(), "models/new/renamed.vmdl_c"),
        history.Versions[0].Paths.end());
    ASSERT_EQ(history.Links.size(), 1u);
    EXPECT_EQ(history.Links[0].Relation, "renamed");
    EXPECT_EQ(history.Links[0].Detail, "rename-only");
    EXPECT_EQ(history.Links[0].From.Bytes, id.Bytes);
    EXPECT_EQ(history.Links[0].To.Bytes, id.Bytes);
    EXPECT_TRUE(store.UnprovenSamePath(id).empty());
}

TEST(Store, DroppingOneObservationKeepsTheSnapshotAndBlobs) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model{8, 8, 8};
    const std::vector<uint8_t> note{1, 2, 3, 4};
    const std::vector<VpkChange> changes{
        VpkChange{"models/a.vmdl_c", model},
        VpkChange{"materials/readme.txt", note},
    };
    const fs::path original = dir.Path() / "original.vpk";
    const fs::path repack = dir.Path() / "repack.vpk";
    ArchiveWith(original, changes);
    ArchiveWith(repack, changes);

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId snapshot = store.ImportVpk(original);
    store.ImportVpk(repack);
    const int blob_files = CountBlobFiles(store_path);
    Catalog catalog(store_path);
    const auto observation_ids = catalog.Texts("SELECT id FROM observations ORDER BY absolute_path");
    ASSERT_EQ(observation_ids.size(), 2u);
    const int64_t snapshots = catalog.Int("SELECT COUNT(*) FROM snapshots");
    const int64_t observations = catalog.Int("SELECT COUNT(*) FROM observations");
    const int64_t memberships = catalog.Int("SELECT COUNT(*) FROM memberships");
    const int64_t blobs = catalog.Int("SELECT COUNT(*) FROM blobs");

    ExpectMessage([&] { store.DropObservation("0123456789abcdef0123456789abcdef"); },
        "The model library has no observation with that id.");
    ExpectMessage([&] { store.DropObservation(""); }, "The model library has no observation with that id.");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), snapshots);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), observations);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM memberships"), memberships);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), blobs);
    EXPECT_EQ(CountBlobFiles(store_path), blob_files);

    store.DropObservation(observation_ids[0]);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM memberships"), memberships);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs"), blobs);
    EXPECT_EQ(CountBlobFiles(store_path), blob_files);
    EXPECT_EQ(catalog.Text("SELECT id FROM observations"), observation_ids[1]);
    EXPECT_EQ(catalog.Text("SELECT id FROM snapshots"), snapshot.Value);
    const std::vector<PackRow> packs = store.ListPacks();
    ASSERT_EQ(packs.size(), 1u);
    EXPECT_EQ(packs[0].ObservationCount, 1);
    EXPECT_EQ(packs[0].Snapshot.Value, snapshot.Value);
    EXPECT_EQ(store.ReadBlob(IdOf(model)), model);
    EXPECT_EQ(store.ReadBlob(IdOf(note)), note);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs WHERE bytes_state = 'dropped'"), 0);
}

TEST(Store, DroppingTheLastObservationKeepsEdgeBlobsThroughGarbageCollection) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const std::vector<uint8_t> model_c{3, 3, 3};
    const std::vector<uint8_t> model_a2{1, 1, 2};
    const BlobId id_a = IdOf(model_a);
    const BlobId id_b = IdOf(model_b);
    const BlobId id_c = IdOf(model_c);
    const BlobId id_a2 = IdOf(model_a2);
    const fs::path vpk = dir.Path() / "mod.vpk";
    ArchiveWith(vpk, {
        VpkChange{"models/a.vmdl_c", model_a},
        VpkChange{"models/b.vmdl_c", model_b},
        VpkChange{"models/c.vmdl_c", model_c},
    });

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId input = store.ImportVpk(vpk);
    RepairRequest repair;
    repair.Input = input;
    repair.OutputPath = "D:/out/repaired.vpk";
    repair.Changes.push_back(RepairChange{"models/a.vmdl_c", "models/a.vmdl_c", model_a2});
    repair.Changes.push_back(RepairChange{"models/c.vmdl_c", "", std::nullopt});
    repair.Models.push_back(RepairModelNote{"models/a.vmdl_c", "Updated", "mutate-a", {}, {}, {}, std::nullopt});
    const SnapshotId output = store.RecordRepair(repair);

    Catalog catalog(store_path);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 2);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 1);
    const std::string observation_id = catalog.Text("SELECT id FROM observations");
    store.DropObservation(observation_id);

    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM snapshots"), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM observations"), 0);
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM snapshots WHERE id = '" + input.Value + "'").c_str()), 0);
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM memberships WHERE snapshot_id = '" + input.Value + "'").c_str()), 0);
    EXPECT_EQ(catalog.Int(("SELECT COUNT(*) FROM refs WHERE snapshot_id = '" + input.Value + "'").c_str()), 0);
    EXPECT_EQ(catalog.Text("SELECT id FROM snapshots"), output.Value);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM memberships WHERE blob_sha256 = ?", id_a.Bytes), 0);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM memberships WHERE blob_sha256 = ?", id_c.Bytes), 0);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM memberships WHERE blob_sha256 = ?", id_b.Bytes), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges"), 1);
    EXPECT_EQ(store.ReadBlob(id_a), model_a);
    EXPECT_EQ(store.ReadBlob(id_b), model_b);
    EXPECT_EQ(store.ReadBlob(id_c), model_c);
    EXPECT_EQ(store.ReadBlob(id_a2), model_a2);

    const History before = store.HistoryOf(id_a2);
    ASSERT_EQ(before.Versions.size(), 2u);
    EXPECT_EQ(before.Versions[0].Blob.Bytes, id_a.Bytes);
    EXPECT_EQ(before.Versions[1].Blob.Bytes, id_a2.Bytes);
    ASSERT_EQ(before.Links.size(), 1u);
    EXPECT_EQ(before.Links[0].Relation, "mutated");
    EXPECT_EQ(before.Links[0].Detail, "mutate-a");

    const fs::path incoming = store_path / "incoming" / "left-behind";
    WriteBytes(incoming, {1, 2, 3});
    const fs::path orphan = store_path / "blobs" / "sha256" / "aa" / std::string(64, 'a');
    fs::create_directories(orphan.parent_path());
    WriteBytes(orphan, {4, 5});
    const std::string removed_hex = id_c.Hex();
    const fs::path removed_blob = store_path / "blobs" / "sha256" / removed_hex.substr(0, 2) / removed_hex;
    EXPECT_TRUE(fs::exists(removed_blob));
    const int files_before = CountBlobFiles(store_path);

    const GcStats stats = store.CollectGarbage();
    EXPECT_EQ(stats.IncomingFilesRemoved, 1);
    EXPECT_EQ(stats.BlobFilesRemoved, 2);
    EXPECT_EQ(stats.BlobRowsRemoved, 1);
    EXPECT_FALSE(fs::exists(incoming));
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_FALSE(fs::exists(removed_blob));
    EXPECT_EQ(CountBlobFiles(store_path), files_before - 2);
    EXPECT_EQ(store.ReadBlob(id_a), model_a);
    EXPECT_EQ(store.ReadBlob(id_a2), model_a2);
    EXPECT_EQ(store.ReadBlob(id_b), model_b);
    ExpectMessage([&] { store.ReadBlob(id_c); }, "The model library has no blob with that id.");
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs WHERE bytes_state = 'dropped'"), 0);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM blobs WHERE bytes_state != 'present'"), 0);
    EXPECT_EQ(catalog.IntWhere("SELECT COUNT(*) FROM blobs WHERE sha256 = ?", id_a.Bytes), 1);
    EXPECT_EQ(catalog.Int("SELECT COUNT(*) FROM edges"), 1);

    const History after = store.HistoryOf(id_a2);
    ASSERT_EQ(after.Versions.size(), 2u);
    EXPECT_EQ(after.Versions[0].Blob.Bytes, id_a.Bytes);
    EXPECT_EQ(after.Versions[1].Blob.Bytes, id_a2.Bytes);
    ASSERT_EQ(after.Links.size(), 1u);
    EXPECT_EQ(after.Links[0].Relation, "mutated");
    EXPECT_EQ(after.Links[0].Detail, "mutate-a");
    EXPECT_EQ(after.Links[0].From.Bytes, id_a.Bytes);
    EXPECT_EQ(after.Links[0].To.Bytes, id_a2.Bytes);

    const History from_edge = store.HistoryOf(id_a);
    ASSERT_EQ(from_edge.Versions.size(), 2u);
    EXPECT_EQ(from_edge.Links[0].Detail, "mutate-a");

    const GcStats again = store.CollectGarbage();
    EXPECT_EQ(again.IncomingFilesRemoved, 0);
    EXPECT_EQ(again.BlobFilesRemoved, 0);
    EXPECT_EQ(again.BlobRowsRemoved, 0);
    EXPECT_EQ(store.ReadBlob(id_a), model_a);
}
