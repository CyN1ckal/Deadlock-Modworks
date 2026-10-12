#include "model_library.hpp"
#include "kv3.hpp"
#include "error.hpp"
#include "mod_store.hpp"
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
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    ASSERT_NE(cached, source);
    ModelLibrary library{{cached}, {{cached, "models/a.vmdl_c"}}, dir / "game.vpk"};
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
TEST_F(LibraryTest, LocalStorePublishesOnlyValidatedImportsAndHonorsCancellation) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore store(dir / "store");
    CancellationSource cancelled; cancelled.Cancel();
    EXPECT_THROW(store.Import(source, cancelled.Token()), OperationCancelled);
    EXPECT_FALSE(fs::exists(store.Root() / "mods"));
    std::ifstream in(source, std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(in), {}); in.close();
    uint32_t tree_size = 0; std::memcpy(&tree_size, bytes.data() + 8, 4);
    bytes[28 + tree_size] ^= 0xff;
    { std::ofstream out(source, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
    EXPECT_THROW(store.Import(source), InvalidData);
    EXPECT_TRUE(fs::is_empty(store.Root() / "mods"));
}
TEST_F(LibraryTest, LocalStoreRestoresBackupAndSavesRemovalWithoutDeletingCopies) {
    const auto source = dir / "source.vpk";
    Archive(source, {{"a.txt", std::vector<uint8_t>{1}}});
    const LocalModStore store(dir / "store");
    const auto cached = store.Import(source);
    const ModelLibrary library{{cached}, {}};
    store.Save(library); store.Save({});
    EXPECT_TRUE(store.Load().Sources.empty());
    EXPECT_TRUE(fs::exists(cached));
    { std::ofstream out(store.Root() / "library.json"); out << "invalid"; }
    EXPECT_EQ(store.Load(), library);
    store.Save(library);
    EXPECT_EQ(store.Load(), library);
    EXPECT_FALSE(fs::is_empty(store.Root() / "mods"));
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
}
