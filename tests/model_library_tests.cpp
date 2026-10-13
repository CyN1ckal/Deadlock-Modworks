#include "model_library.hpp"
#include "kv3.hpp"
#include "error.hpp"
#include "mod_store.hpp"
#include "strings.hpp"
#include <nlohmann/json.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <map>
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <cstring>

namespace {
using namespace skeleton_repair;
namespace fs = std::filesystem;
class LibraryTest : public testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("model-library-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { fs::create_directories(dir); }
    void TearDown() override { std::error_code error; fs::remove_all(dir, error); }
    void Archive(const fs::path& path, const std::vector<VpkChange>& entries) {
        std::vector<uint8_t> seed(29);
        Put(seed, 0, 0x55aa1234); Put(seed, 4, 2); Put(seed, 8, 1);
        { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(seed.data()), seed.size()); }
        VpkArchive::Open(path).Write(path, entries);
    }
    static void Put(std::vector<uint8_t>& bytes, size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); }
    static void Execute(const fs::path& database, const std::string& sql) {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(Narrow(database.wstring()).c_str(), &db), SQLITE_OK);
        EXPECT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK) << sqlite3_errmsg(db);
        sqlite3_close(db);
    }
    static int64_t Count(const LocalModStore& store, const std::string& table) {
        sqlite3* db = nullptr;
        sqlite3_stmt* statement = nullptr;
        int64_t count = -1;
        if (sqlite3_open(Narrow(store.Database().wstring()).c_str(), &db) == SQLITE_OK
            && sqlite3_prepare_v2(db, ("SELECT count(*) FROM " + table).c_str(), -1, &statement, nullptr) == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_ROW) count = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return count;
    }
    static int QueryInt(const fs::path& database, const std::string& sql) {
        sqlite3* db = nullptr;
        sqlite3_stmt* statement = nullptr;
        int value = -1;
        if (sqlite3_open(Narrow(database.wstring()).c_str(), &db) == SQLITE_OK
            && sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_ROW) value = sqlite3_column_int(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(db);
        return value;
    }
    static const VpkEntry* FindEntry(const VpkArchive& archive, const std::string& path) {
        const auto& entries = archive.Entries();
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.FullPath == path; });
        return found == entries.end() ? nullptr : &*found;
    }
    // Every file under `folder` with its contents, skipping the model library and rebuilt copies, or only files named `only`.
    static std::map<std::string, std::string> Snapshot(const fs::path& folder, const std::string& only = {}) {
        std::map<std::string, std::string> files;
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (!entry.is_regular_file()) continue;
            const auto relative = entry.path().lexically_relative(folder);
            if (!only.empty() ? entry.path().filename() != only
                : relative.begin()->string() == "cache" || relative.begin()->string() == "store") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            files[relative.string()] = std::string(std::istreambuf_iterator<char>(in), {});
        }
        return files;
    }
    static std::vector<uint8_t> Resource(const std::vector<std::string>& references = {}, int variant = 0) {
        KvDocument doc;
        std::vector<KvValue> names;
        for (const auto& reference : references) {
            auto value = KvValue::String(reference); value.Flag = KvFlag::Resource; names.push_back(std::move(value));
        }
        doc.Root.Set("dependencies", KvValue::Array(std::move(names)));
        doc.Root.Set("variant", KvValue::Int32(variant));
        const auto data = WriteKv3(doc);
        std::vector<uint8_t> bytes(28 + data.size());
        Put(bytes, 0, static_cast<uint32_t>(bytes.size())); bytes[4] = 12;
        Put(bytes, 8, 8); Put(bytes, 12, 1);
        std::memcpy(bytes.data() + 16, "DATA", 4); Put(bytes, 20, 8); Put(bytes, 24, static_cast<uint32_t>(data.size()));
        std::copy(data.begin(), data.end(), bytes.begin() + 28);
        return bytes;
    }
    static std::vector<uint8_t> RerlResource(const std::string& reference) {
        auto data = Resource();
        const auto payload = std::vector<uint8_t>(data.begin() + 28, data.end());
        std::vector<uint8_t> bytes(40 + payload.size() + 24 + reference.size() + 1);
        Put(bytes, 0, static_cast<uint32_t>(bytes.size())); bytes[4] = 12;
        Put(bytes, 8, 8); Put(bytes, 12, 2);
        std::memcpy(bytes.data() + 16, "DATA", 4); Put(bytes, 20, 20); Put(bytes, 24, static_cast<uint32_t>(payload.size()));
        std::memcpy(bytes.data() + 28, "RERL", 4); Put(bytes, 32, static_cast<uint32_t>(8 + payload.size()));
        Put(bytes, 36, static_cast<uint32_t>(24 + reference.size() + 1));
        std::copy(payload.begin(), payload.end(), bytes.begin() + 40);
        const auto start = 40 + payload.size();
        Put(bytes, start, 8); Put(bytes, start + 4, 1); Put(bytes, start + 16, 8);
        std::copy(reference.begin(), reference.end(), bytes.begin() + start + 24);
        return bytes;
    }
};
TEST_F(LibraryTest, ExportsOnlySelectionsAndRecursiveDependencies) {
    const auto source = dir / "source.vpk", output = dir / "output.vpk";
    const auto model = Resource({"materials/hero.vmat"});
    const auto material = Resource({"textures/hero.vtex"});
    const auto texture = Resource();
    Archive(source, {{"models/a.vmdl_c", model}, {"models/b.vmdl_c", Resource()}, {"materials/hero.vmat_c", material}, {"textures/hero.vtex_c", texture}, {"unrelated.txt", std::vector<uint8_t>{1}}});
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, {}, output), 3u);
    const auto result = VpkArchive::Open(output);
    ASSERT_EQ(result.Entries().size(), 3u);
    for (const auto& entry : result.Entries()) EXPECT_NE(entry.FullPath, "models/b.vmdl_c");
    EXPECT_EQ(VpkArchive::Open(source).Entries().size(), 5u);
}
TEST_F(LibraryTest, DeduplicatesSharedFilesAndHandlesCycles) {
    const auto a = dir / "a.vpk", b = dir / "b.vpk";
    const auto shared = Resource({"textures/shared.vtex"});
    Archive(a, {{"models/a.vmdl_c", Resource({"textures/shared.vtex"})}, {"textures/shared.vtex_c", shared}});
    Archive(b, {{"models/b.vmdl_c", Resource({"Textures/Shared.vtex"})}, {"Textures/Shared.vtex_c", shared}});
    EXPECT_EQ(ExportModelLibrary({{a, "models/a.vmdl_c"}, {b, "models/b.vmdl_c"}}, {}, dir / "output.vpk"), 3u);
}
TEST_F(LibraryTest, ConflictsBlockPublicationAndIdentifySources) {
    const auto a = dir / "a.vpk", b = dir / "b.vpk", output = dir / "output.vpk";
    Archive(a, {{"models/a.vmdl_c", Resource({"materials/shared.vmat"})}, {"materials/shared.vmat_c", Resource({}, 1)}});
    Archive(b, {{"models/b.vmdl_c", Resource({"materials/shared.vmat"})}, {"materials/shared.vmat_c", Resource({}, 2)}});
    try {
        ExportModelLibrary({{a, "models/a.vmdl_c"}, {b, "models/b.vmdl_c"}}, {}, output);
        FAIL() << "Expected conflict";
    } catch (const InvalidData& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("materials/shared.vmat_c"), std::string::npos);
        EXPECT_NE(message.find("a.vpk"), std::string::npos); EXPECT_NE(message.find("b.vpk"), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(output));
}
TEST_F(LibraryTest, TraversesGameDependenciesToFindSourceOverridesWithoutPackingGameFiles) {
    const auto source = dir / "source.vpk", game = dir / "game.vpk", output = dir / "output.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource({"materials/game.vmat"})}, {"textures/override.vtex_c", Resource({}, 9)}});
    Archive(game, {{"materials/game.vmat_c", Resource({"textures/override.vtex"})}, {"textures/override.vtex_c", Resource()}});
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, game, output), 2u);
    const auto result = VpkArchive::Open(output);
    for (const auto& entry : result.Entries()) EXPECT_NE(entry.FullPath, "materials/game.vmat_c");
}
TEST_F(LibraryTest, MissingDependenciesAndUnavailableSelectionsBlockExport) {
    const auto source = dir / "source.vpk", output = dir / "output.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource({"materials/missing.vmat"})}});
    EXPECT_THROW(ExportModelLibrary({{source, "models/a.vmdl_c"}}, {}, output), InvalidData);
    EXPECT_THROW(ExportModelLibrary({{source, "models/missing.vmdl_c"}}, {}, output), InvalidData);
    EXPECT_FALSE(fs::exists(output));
}
TEST_F(LibraryTest, ConnectedGameSkipsStaleReferencesFoundNowhere) {
    const auto source = dir / "source.vpk", game = dir / "game.vpk", output = dir / "output.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource({"models/a.vanmgrph", "materials/a.vmat"})}, {"materials/a.vmat_c", Resource({"textures/stale.vtex"})}});
    Archive(game, {{"unrelated.txt", std::vector<uint8_t>{1}}});
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, game, output), 2u);
    EXPECT_THROW(ExportModelLibrary({{source, "models/missing.vmdl_c"}}, game, dir / "missing.vpk"), InvalidData);
}
TEST_F(LibraryTest, RefusesOverwritesAndCancellationDoesNotPublish) {
    const auto source = dir / "source.vpk", output = dir / "output.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource()}});
    const std::vector<LibraryModel> models{{source, "models/a.vmdl_c"}};
    EXPECT_THROW(ExportModelLibrary(models, {}, source), InvalidData);
    CancellationSource cancel; cancel.Cancel();
    EXPECT_THROW(ExportModelLibrary(models, {}, output, cancel.Token()), OperationCancelled);
    EXPECT_FALSE(fs::exists(output));
    Archive(output, {{"existing.txt", std::vector<uint8_t>{4}}});
    EXPECT_THROW(ExportModelLibrary(models, {}, output), IoError);
    EXPECT_EQ(VpkArchive::Open(output).Entries().front().FullPath, "existing.txt");
}
TEST_F(LibraryTest, LibraryRoundTripsSelectionsAndRejectsUnknownSources) {
    const auto source = dir / "reference.vpk", project = dir / "library.json";
    ModelLibrary{{source}, {{source, "models/hero.vmdl_c"}}}.Save(project);
    const auto loaded = ModelLibrary::Load(project);
    ASSERT_EQ(loaded.Sources.size(), 1u); ASSERT_EQ(loaded.Selected.size(), 1u);
    EXPECT_EQ(loaded.Sources.front(), source);
    EXPECT_EQ(loaded.Selected.front().Model, "models/hero.vmdl_c");
    ModelLibrary{{source}, {{dir / "other.vpk", "models/hero.vmdl_c"}}}.Save(project);
    EXPECT_THROW(ModelLibrary::Load(project), InvalidData);
    EXPECT_THROW((ModelLibrary{{source}, {}}.Save(source)), InvalidData);
}
TEST_F(LibraryTest, IncludesRerlDependenciesAndPlainSkeletonReferences) {
    const auto source = dir / "source.vpk";
    auto model = Resource();
    KvDocument doc;
    doc.Root.Set("m_vecNmSkeletonRefs", KvValue::Array({KvValue::String("models/rig.vnmskel")}));
    const auto data = WriteKv3(doc);
    model.resize(28 + data.size()); Put(model, 0, static_cast<uint32_t>(model.size())); Put(model, 24, static_cast<uint32_t>(data.size()));
    std::copy(data.begin(), data.end(), model.begin() + 28);
    Archive(source, {{"models/a.vmdl_c", model}, {"models/rig.vnmskel_c", RerlResource("textures/skin.vtex")}, {"textures/skin.vtex_c", Resource()}});
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, {}, dir / "output.vpk"), 3u);
}
TEST_F(LibraryTest, LocalStoreKeepsImportedModsAfterOriginalIsRemoved) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource()}});
    Archive(dir / "game.vpk", {});
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    ASSERT_NE(cached, source);
    ModelLibrary library{{cached}, {{cached, "models/a.vmdl_c"}}, fs::absolute(dir / "game.vpk").lexically_normal()};
    store.Save(library);
    fs::remove(source);
    const LocalModStore restarted(dir / "store");
    EXPECT_EQ(restarted.Load(), library);
    EXPECT_EQ(restarted.Import(cached), cached);
    VpkArchive::Open(cached).Validate();
    EXPECT_EQ(ExportModelLibrary(restarted.Load().Selected, {}, dir / "combined.vpk"), 1u);
}
TEST_F(LibraryTest, LocalStoreDeduplicatesRepeatImportsAndSeparatesSameNames) {
    fs::create_directories(dir / "one"); fs::create_directories(dir / "two");
    const auto one = dir / "one" / "mod.vpk", two = dir / "two" / "mod.vpk";
    Archive(one, {{"a.txt", std::vector<uint8_t>{1}}});
    Archive(two, {{"a.txt", std::vector<uint8_t>{2}}});
    const LocalModStore store(dir / "store");
    const auto first = store.Import(one);
    EXPECT_EQ(store.Import(one), first);
    const auto second = store.Import(two);
    EXPECT_NE(first, second);
    const auto archive = VpkArchive::Open(second);
    EXPECT_EQ(archive.Read(archive.Entries().front()), (std::vector<uint8_t>{2}));
}
TEST_F(LibraryTest, LocalStoreDeduplicatesIdenticalContentByChecksum) {
    fs::create_directories(dir / "elsewhere");
    const auto one = dir / "mod.vpk", copy = dir / "elsewhere" / "renamed.vpk", other = dir / "other.vpk";
    const std::vector<uint8_t> shared(4096, 7);
    Archive(one, {{"a.txt", std::vector<uint8_t>{1}}, {"shared.bin", shared}});
    fs::copy_file(one, copy);
    Archive(other, {{"b.txt", std::vector<uint8_t>{2}}, {"shared.bin", shared}});
    const LocalModStore store(dir / "store");
    const auto first = store.Import(one);
    // A copy under another name and path is the same stored mod.
    EXPECT_EQ(store.Import(copy), first);
    store.Import(other);
    EXPECT_EQ(Count(store, "snapshots"), 2);
    // a.txt, b.txt, and one shared.bin for both mods.
    EXPECT_EQ(Count(store, "blobs"), 3);
}
TEST_F(LibraryTest, LocalStoreNamesCopiesAfterTheModAndRenamesThemOnLoad) {
    fs::create_directories(dir / "addons");
    const auto source = dir / "addons" / "pak02_dir.vpk";
    Archive(source, {{"models/heroes_staging/haze/haze.vmdl_c", Resource()}});
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    EXPECT_EQ(cached.filename(), fs::path("haze.vpk"));
    EXPECT_EQ(store.Name(cached), std::optional<std::string>("haze"));
    EXPECT_EQ(store.Name(source), std::nullopt);
    store.Save({{cached}, {{cached, "models/heroes_staging/haze/haze.vmdl_c"}}});

    // A rename keeps paths already handed out until the library is loaded again.
    store.Rename(cached, "Haze: maid outfit");
    EXPECT_EQ(store.Import(cached), cached);
    const auto loaded = store.Load();
    ASSERT_EQ(loaded.Sources.size(), 1u);
    const auto renamed = loaded.Sources.front();
    EXPECT_EQ(renamed.filename(), fs::path("Haze_ maid outfit.vpk"));
    EXPECT_EQ(renamed.parent_path(), cached.parent_path());
    EXPECT_FALSE(fs::exists(cached));
    ASSERT_EQ(loaded.Selected.size(), 1u);
    EXPECT_EQ(loaded.Selected.front().Archive, renamed);
    VpkArchive::Open(renamed).Validate();
    EXPECT_EQ(store.Name(renamed), std::optional<std::string>("Haze: maid outfit"));

    store.Rename(renamed, "");
    EXPECT_EQ(store.Load().Sources.front().filename(), fs::path("haze.vpk"));
}
TEST_F(LibraryTest, LocalStoreChangedOriginalCreatesAnotherVersion) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore store(dir / "store");
    const auto first = store.Import(source);
    Archive(source, {{"a.txt", std::vector<uint8_t>{2, 3}}});
    const auto second = store.Import(source);
    EXPECT_NE(first, second);
    VpkArchive::Open(first).Validate(); VpkArchive::Open(second).Validate();
}
TEST_F(LibraryTest, LocalStoreStoresOnlyValidatedImportsAndHonorsCancellation) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore store(dir / "store");
    CancellationSource cancelled; cancelled.Cancel();
    EXPECT_THROW(store.Import(source, cancelled.Token()), OperationCancelled);
    std::ifstream in(source, std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(in), {}); in.close();
    uint32_t tree_size = 0; std::memcpy(&tree_size, bytes.data() + 8, 4);
    bytes[28 + tree_size] ^= 0xff;
    { std::ofstream out(source, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    EXPECT_THROW(store.Import(source), InvalidData);
    EXPECT_EQ(Count(store, "snapshots"), 0);
    EXPECT_FALSE(fs::exists(store.Root() / "cache"));
}
TEST_F(LibraryTest, LocalStoreSavesRemovalWithoutDeletingStoredMods) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    store.Save({{cached}, {}});
    store.Save({});
    EXPECT_TRUE(store.Load().Sources.empty());
    EXPECT_EQ(Count(store, "snapshots"), 1);
    fs::remove(source);
    // Still stored: the original import is recognized and rebuilt without the original file.
    Archive(dir / "again.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    fs::remove_all(store.Root() / "cache");
    EXPECT_EQ(store.Import(dir / "again.vpk"), cached);
    VpkArchive::Open(cached).Validate();
}
TEST_F(LibraryTest, LocalStoreRebuildsMissingOrRepairedCopiesFromTheDatabase) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource()}, {"a.txt", std::vector<uint8_t>{1, 2}}});
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    store.Save({{cached}, {{cached, "models/a.vmdl_c"}}});
    fs::remove_all(store.Root() / "cache");
    const auto restored = store.Load();
    ASSERT_EQ(restored.Sources, std::vector<fs::path>{cached});
    const auto archive = VpkArchive::Open(cached);
    archive.Validate();
    EXPECT_EQ(archive.Entries().size(), 2u);
    // A copy changed in place, as a repair does, is stored as a new version and the original stays.
    Archive(cached, {{"models/a.vmdl_c", Resource({}, 1)}, {"a.txt", std::vector<uint8_t>{1, 2}}});
    const auto repaired = store.Import(cached);
    EXPECT_NE(repaired, cached);
    EXPECT_EQ(Count(store, "snapshots"), 2);
    EXPECT_EQ(store.Import(source), cached);
    const auto rebuilt = VpkArchive::Open(cached);
    rebuilt.Validate();
    EXPECT_EQ(rebuilt.Read(*std::find_if(rebuilt.Entries().begin(), rebuilt.Entries().end(), [](const auto& e) { return e.FullPath == "models/a.vmdl_c"; })), Resource());
    EXPECT_EQ(Count(store, "blobs"), 3);
}
TEST_F(LibraryTest, LocalStoreMergesEarlierFolderStoreWithoutChangingIt) {
    const auto root = dir / "store";
    const auto original = dir / "original.vpk";
    Archive(original, {{"models/a.vmdl_c", Resource()}});
    fs::create_directories(root / "mods" / "0123");
    const auto legacy = root / "mods" / "0123" / "original.vpk";
    fs::copy_file(original, legacy);
    {
        std::ofstream out(root / "mods" / "0123" / "origin.json");
        out << nlohmann::json{{"original", Narrow(original.wstring())}, {"size", fs::file_size(original)},
            {"stamp", fs::last_write_time(original).time_since_epoch().count()}, {"chunks", nlohmann::json::array()}}.dump();
    }
    Archive(dir / "game.vpk", {});
    ModelLibrary{{legacy}, {{legacy, "models/a.vmdl_c"}}, dir / "game.vpk"}.Save(root / "library.json");
    const auto before = Snapshot(root);
    const LocalModStore store(root);
    std::string notice;
    const auto library = store.Load({}, {}, &notice);
    EXPECT_EQ(notice, "");
    ASSERT_EQ(library.Sources.size(), 1u);
    EXPECT_EQ(library.Selected, (std::vector<LibraryModel>{{library.Sources[0], "models/a.vmdl_c"}}));
    EXPECT_EQ(library.GameArchive, fs::absolute(dir / "game.vpk").lexically_normal());
    EXPECT_EQ(Snapshot(root), before);
    EXPECT_EQ(store.LegacyPaths(library.Sources[0]).size(), 1u);
    // The original is recognized from its import record.
    EXPECT_EQ(store.Import(original), library.Sources[0]);
    // Merged once: a source removed afterwards is not added back.
    store.Save({});
    EXPECT_TRUE(store.Load().Sources.empty());
    EXPECT_EQ(Count(store, "snapshots"), 1);
}
TEST_F(LibraryTest, LocalStoreExportsAndMergesSharedLibraries) {
    const auto one = dir / "one.vpk", two = dir / "two.vpk";
    Archive(one, {{"models/a.vmdl_c", Resource()}});
    Archive(two, {{"models/b.vmdl_c", Resource({}, 1)}});
    const LocalModStore mine(dir / "mine"), theirs(dir / "theirs");
    const auto a = mine.Import(one);
    mine.Save({{a}, {{a, "models/a.vmdl_c"}}});
    theirs.SaveThumbnail("thumb", {1, 2, 3});
    const auto b = theirs.Import(two);
    const auto shared_a = theirs.Import(one);
    theirs.Save({{b, shared_a}, {{b, "models/b.vmdl_c"}, {shared_a, "models/a.vmdl_c"}}});
    const auto file = dir / "shared library.db";
    theirs.Export(file);
    const auto exported = Snapshot(dir, "shared library.db");
    fs::remove(two); fs::remove(one);
    mine.Merge(file);
    mine.Merge(file);
    EXPECT_EQ(Snapshot(dir, "shared library.db"), exported);
    const auto merged = mine.Load();
    ASSERT_EQ(merged.Sources.size(), 2u);
    EXPECT_EQ(merged.Sources[0], a);
    EXPECT_EQ(merged.Selected.size(), 2u);
    EXPECT_EQ(Count(mine, "snapshots"), 2);
    EXPECT_EQ(mine.Thumbnail("thumb"), (std::vector<uint8_t>{1, 2, 3}));
    VpkArchive::Open(merged.Sources[1]).Validate();
    EXPECT_THROW(mine.Merge(dir / "missing.db"), std::exception);
}
TEST_F(LibraryTest, LocalStoreMergesPreviousSingleFileLibraryOnce) {
    const auto one = dir / "one.vpk";
    Archive(one, {{"models/a.vmdl_c", Resource()}});
    const LocalModStore previous(dir / "previous");
    const auto a = previous.Import(one);
    previous.Save({{a}, {{a, "models/a.vmdl_c"}}});
    previous.SaveThumbnail("thumb", {4, 5});
    // The previous release kept the whole library in library.db beside the store folder.
    const auto root = dir / "store";
    fs::create_directories(root);
    previous.Export(root / "library.db");
    const auto before = Snapshot(root, "library.db");
    fs::remove(one);
    const LocalModStore store(root);
    std::string notice;
    const auto library = store.Load({}, {}, &notice);
    EXPECT_EQ(notice, "");
    ASSERT_EQ(library.Sources.size(), 1u);
    EXPECT_EQ(library.Selected, (std::vector<LibraryModel>{{library.Sources[0], "models/a.vmdl_c"}}));
    EXPECT_EQ(store.Thumbnail("thumb"), (std::vector<uint8_t>{4, 5}));
    EXPECT_EQ(Snapshot(root, "library.db"), before);
    VpkArchive::Open(library.Sources[0]).Validate();
    // Merged once: a source removed afterwards is not added back.
    store.Save({});
    EXPECT_TRUE(store.Load().Sources.empty());
}
TEST_F(LibraryTest, LocalStoreRejectsSharedLibraryWithMismatchedChecksums) {
    const auto one = dir / "one.vpk";
    Archive(one, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore mine(dir / "mine"), theirs(dir / "theirs");
    theirs.Import(one);
    const auto file = dir / "shared.db";
    theirs.Export(file);
    Execute(file, "UPDATE archive_entries SET path = 'b.txt'");
    EXPECT_THROW(mine.Merge(file), InvalidData);
    EXPECT_EQ(Count(mine, "snapshots"), 0);
}
TEST_F(LibraryTest, LocalStoreFlattensChunkedVpkIntoSelfContainedCopy) {
    const auto source = dir / "chunked_dir.vpk", chunk = dir / "chunked_000.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1, 2, 3}}});
    std::ifstream in(source, std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(in), {}); in.close();
    uint32_t tree_size = 0; std::memcpy(&tree_size, bytes.data() + 8, 4);
    const size_t entry = 28 + 4 + 2 + 2; // txt, root marker, filename a.
    bytes[entry + 6] = 0; bytes[entry + 7] = 0;
    { std::ofstream out(chunk, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data() + 28 + tree_size), 3); }
    Put(bytes, 12, 0); Put(bytes, 16, 0); Put(bytes, 20, 0); Put(bytes, 24, 0);
    bytes.resize(28 + tree_size);
    { std::ofstream out(source, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    fs::remove(source); fs::remove(chunk);
    const auto archive = VpkArchive::Open(cached);
    ASSERT_EQ(archive.Entries().size(), 1u);
    EXPECT_EQ(archive.Entries().front().ArchiveIndex, VpkArchive::EmbeddedArchive);
    EXPECT_EQ(archive.Read(archive.Entries().front()), (std::vector<uint8_t>{1, 2, 3}));
}
TEST_F(LibraryTest, PackageExportsTexturesWithoutAModelOrGameArchive) {
    const auto source = dir / "portraits.vpk", output = dir / "output.vpk";
    const std::vector<uint8_t> small{1, 2, 3, 4};
    const std::vector<uint8_t> large{5, 6, 7, 8, 9};
    Archive(source, {{"models/hideout/materials/small.vtex_c", small}, {"models/hideout/materials/large.vtex_c", large}});
    // A missing game archive must not be opened: these files are copied as stored.
    EXPECT_EQ(ExportModelLibrary({}, dir / "missing-game.vpk", output, {}, {source}), 2u);
    const auto result = VpkArchive::Open(output);
    ASSERT_EQ(result.Entries().size(), 2u);
    const auto* first = FindEntry(result, "models/hideout/materials/small.vtex_c");
    const auto* second = FindEntry(result, "models/hideout/materials/large.vtex_c");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(result.Read(*first), small);
    EXPECT_EQ(result.Read(*second), large);
}
TEST_F(LibraryTest, PackageAddsUnreferencedFilesWithoutTheUnselectedModel) {
    const auto source = dir / "source.vpk";
    const std::vector<uint8_t> loose{9, 9, 9};
    Archive(source, {{"models/a.vmdl_c", Resource()}, {"models/b.vmdl_c", Resource({}, 1)}, {"textures/loose.vtex_c", loose}});
    const auto model_only = dir / "model-only.vpk";
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, {}, model_only), 1u);
    const auto only = VpkArchive::Open(model_only);
    ASSERT_EQ(only.Entries().size(), 1u);
    EXPECT_EQ(only.Entries().front().FullPath, "models/a.vmdl_c");

    const auto output = dir / "with-package.vpk";
    EXPECT_EQ(ExportModelLibrary({{source, "models/a.vmdl_c"}}, {}, output, {}, {source}), 2u);
    const auto result = VpkArchive::Open(output);
    ASSERT_EQ(result.Entries().size(), 2u);
    EXPECT_EQ(FindEntry(result, "models/b.vmdl_c"), nullptr);
    const auto* texture = FindEntry(result, "textures/loose.vtex_c");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(result.Read(*texture), loose);
}
TEST_F(LibraryTest, PackageConflictsNameBothSourcesAndIdenticalBytesExportOnce) {
    const auto a = dir / "a.vpk", b = dir / "b.vpk", output = dir / "output.vpk";
    const std::vector<uint8_t> shared{1, 2, 3};
    Archive(a, {{"textures/shared.vtex_c", shared}});
    Archive(b, {{"textures/shared.vtex_c", std::vector<uint8_t>{4, 5, 6}}});
    try {
        ExportModelLibrary({}, {}, output, {}, {a, b});
        FAIL() << "Expected conflict";
    } catch (const InvalidData& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("textures/shared.vtex_c"), std::string::npos);
        EXPECT_NE(message.find("a.vpk"), std::string::npos);
        EXPECT_NE(message.find("b.vpk"), std::string::npos);
        EXPECT_NE(message.find("Deselect one of the models using this file."), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(output));

    Archive(b, {{"textures/shared.vtex_c", shared}});
    EXPECT_EQ(ExportModelLibrary({}, {}, output, {}, {a, b}), 1u);
    const auto result = VpkArchive::Open(output);
    ASSERT_EQ(result.Entries().size(), 1u);
    EXPECT_EQ(result.Read(result.Entries().front()), shared);
}
TEST_F(LibraryTest, PackageOfOnlyModelsAndAnEmptySelectionFailClearly) {
    const auto source = dir / "source.vpk", output = dir / "output.vpk";
    Archive(source, {{"models/a.vmdl_c", Resource()}});
    try {
        ExportModelLibrary({}, {}, output, {}, {source});
        FAIL() << "Expected a model-only package to fail";
    } catch (const InvalidData& error) {
        EXPECT_NE(std::string(error.what()).find("has no non-model files."), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(output));
    try {
        ExportModelLibrary({}, {}, output);
        FAIL() << "Expected an empty pack to fail";
    } catch (const InvalidData& error) {
        EXPECT_NE(std::string(error.what()).find("Select at least one model."), std::string::npos);
    }
    try {
        ExportModelLibrary({}, {}, output, {}, {dir / "missing.vpk"});
        FAIL() << "Expected a missing package to fail";
    } catch (const InvalidData& error) {
        EXPECT_NE(std::string(error.what()).find("missing.vpk"), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(output));
}
TEST_F(LibraryTest, PackageSubtitleCountsHideoutTexturesApartFromOtherFiles) {
    EXPECT_EQ(PackageSubtitle({
        "models/hideout/materials/one.vtex_c",
        "models/hideout/materials/two.vtex_c",
        "models/hideout/materials/three.vtex_c",
    }), "3 hideout textures");
    EXPECT_EQ(PackageSubtitle({"materials/hero.vtex_c"}), "1 texture");
    EXPECT_EQ(PackageSubtitle({"panorama/layout.vxml_c", "sounds/voice.vsnd_c"}), "1 UI file, 1 sound");
}
TEST_F(LibraryTest, LibraryJsonWithoutPackagesLoadsEmptyAndRoundTripsTheList) {
    const auto source = dir / "portraits.vpk", project = dir / "library.json";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    {
        std::ofstream out(project, std::ios::binary);
        out << nlohmann::json{{"schemaVersion", 1}, {"sources", nlohmann::json::array({Narrow(source.wstring())})},
            {"selected", nlohmann::json::array()}}.dump(2);
    }
    const auto legacy = ModelLibrary::Load(project);
    EXPECT_TRUE(legacy.Packages.empty());
    ASSERT_EQ(legacy.Sources.size(), 1u);
    EXPECT_EQ(legacy.Sources.front(), fs::absolute(source).lexically_normal());

    ModelLibrary{{source}, {}, {}, {source}}.Save(project);
    const auto loaded = ModelLibrary::Load(project);
    ASSERT_EQ(loaded.Packages.size(), 1u);
    EXPECT_EQ(loaded.Packages.front(), fs::absolute(source).lexically_normal());
    const auto saved = nlohmann::json::parse(std::ifstream(project));
    EXPECT_EQ(saved.at("schemaVersion").get<int>(), 1);
    EXPECT_TRUE(saved.contains("packages"));

    ModelLibrary{{source}, {}, {}, {dir / "other.vpk"}}.Save(project);
    EXPECT_THROW(ModelLibrary::Load(project), InvalidData);
}
TEST_F(LibraryTest, LocalStoreSavesAndReloadsAPackageSelection) {
    const auto hero = dir / "hero.vpk", portraits = dir / "portraits.vpk";
    Archive(hero, {{"models/a.vmdl_c", Resource()}});
    Archive(portraits, {{"models/hideout/materials/a.vtex_c", std::vector<uint8_t>{1, 2}}, {"sounds/voice.vsnd_c", std::vector<uint8_t>{3}}});
    const LocalModStore store(dir / "store");
    const auto model = store.Import(hero);
    const auto package = store.Import(portraits);
    const ModelLibrary library{{model, package}, {{model, "models/a.vmdl_c"}}, {}, {package}};
    store.Save(library);
    EXPECT_EQ(LocalModStore(dir / "store").Load(), library);
    // An older catalog has no package table. Opening it creates the table and the selection is kept.
    Execute(store.Database(), "DROP TABLE gallery_packages");
    store.Save(library);
    EXPECT_EQ(Count(store, "gallery_packages"), 1);
    EXPECT_EQ(store.Load(), library);
}
TEST_F(LibraryTest, SharedLibraryVersionTwoRestoresPackagesAndVersionOneDoesNot) {
    const auto hero = dir / "hero.vpk", portraits = dir / "portraits.vpk";
    const std::vector<uint8_t> texture{7, 8, 9};
    Archive(hero, {{"models/a.vmdl_c", Resource()}});
    Archive(portraits, {{"models/hideout/materials/a.vtex_c", texture}});
    const LocalModStore mine(dir / "mine");
    const auto model = mine.Import(hero);
    const auto package = mine.Import(portraits);
    mine.Save({{model, package}, {{model, "models/a.vmdl_c"}}, {}, {package}});
    const auto file = dir / "shared.db";
    mine.Export(file);
    EXPECT_EQ(QueryInt(file, "PRAGMA user_version"), 2);

    const LocalModStore other(dir / "other");
    other.Merge(file);
    const auto merged = other.Load();
    ASSERT_EQ(merged.Sources.size(), 2u);
    ASSERT_EQ(merged.Selected.size(), 1u);
    EXPECT_EQ(merged.Selected.front().Model, "models/a.vmdl_c");
    EXPECT_EQ(merged.Selected.front().Archive, merged.Sources[0]);
    ASSERT_EQ(merged.Packages.size(), 1u);
    EXPECT_EQ(merged.Packages.front(), merged.Sources[1]);
    const auto restored = VpkArchive::Open(merged.Packages.front());
    const auto* entry = FindEntry(restored, "models/hideout/materials/a.vtex_c");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(restored.Read(*entry), texture);
    EXPECT_EQ(FindEntry(restored, "models/a.vmdl_c"), nullptr);

    Execute(file, "DROP TABLE library_packages; PRAGMA user_version = 1;");
    EXPECT_EQ(QueryInt(file, "PRAGMA user_version"), 1);
    const LocalModStore previous(dir / "previous");
    previous.Merge(file);
    const auto legacy = previous.Load();
    ASSERT_EQ(legacy.Sources.size(), 2u);
    ASSERT_EQ(legacy.Selected.size(), 1u);
    EXPECT_EQ(legacy.Selected.front().Model, "models/a.vmdl_c");
    EXPECT_TRUE(legacy.Packages.empty());
    EXPECT_NE(FindEntry(VpkArchive::Open(legacy.Sources[1]), "models/hideout/materials/a.vtex_c"), nullptr);
}

// Path only. DefaultRoot does not open the directory, and this test must not touch the live library.
TEST(Library, DefaultDataRootIsDeadlockModManager) {
    const fs::path root = LocalModStore::DefaultRoot();
    const std::wstring text = root.wstring();
    EXPECT_EQ(root.filename().wstring(), L"DeadlockModManager");
    EXPECT_NE(text.find(L"\\DeadlockModManager"), std::wstring::npos);
    EXPECT_EQ(text.find(L"DeadlockModworks"), std::wstring::npos);
}

// Opens only the directory passed in. The decoy path and DefaultRoot() stay closed.
TEST(Library, ExplicitDataRootIsWhatTheStoreOpens) {
    const fs::path root = fs::temp_directory_path() / ("modworks-data-root-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path decoy = fs::temp_directory_path() / ("modworks-data-root-decoy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    fs::create_directories(root);

    const fs::path chosen = ResolveDataRoot(root, decoy.wstring().c_str());
    EXPECT_EQ(chosen, root);
    {
        LocalModStore store(chosen);
        EXPECT_EQ(store.Root(), fs::absolute(root).lexically_normal());
        EXPECT_EQ(store.Root().wstring().find(L"DeadlockModManager"), std::wstring::npos);
        EXPECT_TRUE(store.Load().Sources.empty());
        EXPECT_TRUE(fs::is_regular_file(store.Database()));
        EXPECT_EQ(store.Database().parent_path().parent_path().lexically_normal(), store.Root());
    }
    EXPECT_FALSE(fs::exists(decoy));
    EXPECT_EQ(ResolveDataRoot(std::nullopt, root.wstring().c_str()), root);
    EXPECT_EQ(ResolveDataRoot(std::nullopt, L""), LocalModStore::DefaultRoot());
}
}
