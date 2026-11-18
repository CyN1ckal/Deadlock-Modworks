#include "library_session.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {

using namespace skeleton_repair;
namespace fs = std::filesystem;

class LibrarySessionTest : public testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("library-jobs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path root = dir / "data";
    LibraryJobProgress progress;

    void SetUp() override { fs::create_directories(dir); }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(dir, error);
    }
    fs::path Archive(const char* name, const std::vector<VpkChange>& entries) {
        const auto path = dir / name;
        std::vector<uint8_t> seed(29);
        const uint32_t header[] = {0x55aa1234, 2, 1};
        std::memcpy(seed.data(), header, sizeof(header));
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        }
        VpkArchive::Open(path).Write(path, entries);
        return path;
    }
    static LibrarySession::Finished WaitFor(LibrarySession& session) {
        session.Wait();
        auto finished = session.Poll();
        EXPECT_TRUE(finished.has_value());
        return finished ? *finished : LibrarySession::Finished{};
    }
};

TEST_F(LibrarySessionTest, SameLibraryPathIgnoresCaseAndDots) {
    EXPECT_TRUE(SameLibraryPath(dir / "Mods" / "A.vpk", dir / "mods" / "." / "x" / ".." / "a.VPK"));
    EXPECT_FALSE(SameLibraryPath(dir / "a.vpk", dir / "b.vpk"));
}

TEST_F(LibrarySessionTest, ImportPutsStoredCopiesInPlaceOfTheirSources) {
    const auto one = Archive("one.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    const auto two = Archive("two.vpk", {{"b.txt", std::vector<uint8_t>{2}}});
    ModelLibrary starting;
    starting.Sources = {one};
    starting.Selected = {{one, "models/a.vmdl_c"}, {dir / "gone.vpk", "models/b.vmdl_c"}};
    starting.Packages = {one};

    const auto result = ImportSources(root, {one, two}, starting, true, {}, progress);
    EXPECT_TRUE(result.Errors.empty()) << result.Errors;
    ASSERT_EQ(result.Imported.size(), 2u);
    const auto& stored_one = result.Imported[0].second;
    const auto& stored_two = result.Imported[1].second;
    EXPECT_EQ(result.Library.Sources, (std::vector<fs::path>{stored_one, stored_two}));
    // The selection of a source that is not in the library is dropped; the others follow their source.
    ASSERT_EQ(result.Library.Selected.size(), 1u);
    EXPECT_EQ(result.Library.Selected[0].Archive, stored_one);
    EXPECT_EQ(result.Library.Packages, (std::vector<fs::path>{stored_one}));
    EXPECT_EQ(progress.Done.load(), 2u);
    EXPECT_EQ(progress.Total.load(), 2u);
    EXPECT_FALSE(result.Restored);
    // Checkpoints saved the collection, so a restore reads it back.
    EXPECT_EQ(RestoreLibrary(root, {}, progress).Library.Sources, result.Library.Sources);
}

TEST_F(LibrarySessionTest, ImportKeepsGoingPastABrokenSource) {
    const auto good = Archive("good.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    const auto result = ImportSources(root, {dir / "missing.vpk", good}, {}, false, {}, progress);
    EXPECT_NE(result.Errors.find("missing.vpk: "), std::string::npos) << result.Errors;
    ASSERT_EQ(result.Imported.size(), 1u);
    EXPECT_EQ(result.Library.Sources.size(), 1u);
}

TEST_F(LibrarySessionTest, CancelledImportStopsAndSaysSo) {
    const auto good = Archive("good.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    CancellationSource cancellation;
    cancellation.Cancel();
    const auto result = ImportSources(root, {good}, {}, false, cancellation.Token(), progress);
    EXPECT_NE(result.Errors.find("Import cancelled"), std::string::npos) << result.Errors;
    EXPECT_TRUE(result.Library.Sources.empty());
}

TEST_F(LibrarySessionTest, RenameReadsTheLibraryBackWithTheNewName) {
    const auto source = Archive("source.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    const auto imported = ImportSources(root, {source}, {}, false, {}, progress);
    LocalModStore(root).Save(imported.Library);
    const auto renamed = RenameStoredMod(root, imported.Library.Sources.at(0), "Shiny", {}, progress);
    EXPECT_TRUE(renamed.Restored);
    EXPECT_EQ(renamed.Done, "Renamed to Shiny.");
    ASSERT_EQ(renamed.Library.Sources.size(), 1u);
    EXPECT_EQ(LocalModStore(root).Name(renamed.Library.Sources[0]), std::optional<std::string>("Shiny"));
}

TEST_F(LibrarySessionTest, SessionRestoresThenSavesOnlyChanges) {
    LibrarySession session(root);
    ASSERT_NE(session.Store(), nullptr);
    EXPECT_TRUE(session.Restoring());
    EXPECT_FALSE(session.CanPersist());
    // Nothing is saved before the saved library has been read.
    EXPECT_EQ(session.Save({}, true), LibrarySession::SaveOutcome::Skipped);

    const auto restored = WaitFor(session);
    EXPECT_EQ(restored.Kind, LibrarySession::Job::Opening);
    EXPECT_TRUE(restored.WasRestoring);
    ASSERT_TRUE(restored.Result.has_value()) << restored.Error;
    EXPECT_FALSE(session.Restoring());
    EXPECT_TRUE(session.CanPersist());
    EXPECT_FALSE(session.Busy());
    EXPECT_EQ(session.Save(restored.Result->Library, false), LibrarySession::SaveOutcome::Unchanged);

    const auto source = Archive("source.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    session.StartImport({source}, restored.Result->Library);
    EXPECT_EQ(session.CurrentJob(), LibrarySession::Job::Importing);
    const auto imported = WaitFor(session);
    ASSERT_TRUE(imported.Result.has_value()) << imported.Error;
    EXPECT_FALSE(imported.WasRestoring);
    EXPECT_EQ(session.Save(imported.Result->Library, false), LibrarySession::SaveOutcome::Saved);
    EXPECT_EQ(session.Save(imported.Result->Library, false), LibrarySession::SaveOutcome::Unchanged);
    EXPECT_EQ(LocalModStore(root).Load().Sources, imported.Result->Library.Sources);
}

TEST_F(LibrarySessionTest, SessionReportsAFailedJob) {
    LibrarySession session(root);
    WaitFor(session);
    session.StartMerge(dir / "not-a-library.db");
    const auto merged = WaitFor(session);
    EXPECT_EQ(merged.Kind, LibrarySession::Job::Merging);
    EXPECT_FALSE(merged.Result.has_value());
    EXPECT_FALSE(merged.Error.empty());
}

}  // namespace
