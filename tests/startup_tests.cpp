#include "history_compare.hpp"
#include "mod_store.hpp"
#include "startup.hpp"
#include "strings.hpp"
#include "vpk.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace skeleton_repair;

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(output));
    output << text;
}

void WriteArchive(const fs::path& path) {
    std::vector<uint8_t> seed(29);
    const uint32_t signature = 0x55aa1234;
    const uint32_t version = 2;
    const uint32_t tree = 1;
    std::memcpy(seed.data(), &signature, 4);
    std::memcpy(seed.data() + 4, &version, 4);
    std::memcpy(seed.data() + 8, &tree, 4);
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(static_cast<bool>(output));
        output.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
    }
    VpkArchive::Open(path).Write(path, {VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1, 2, 3}}});
}

class StartupTest : public testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("modworks-startup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path previous = fs::current_path();
    void SetUp() override { fs::create_directories(dir); }
    void TearDown() override {
        std::error_code error;
        fs::current_path(previous, error);
        fs::remove_all(dir, error);
    }
};

TEST_F(StartupTest, DataPathsStayUnderTheRootWhenThePathHasASpace) {
    std::error_code cwd_error;
    fs::current_path(dir, cwd_error);
    ASSERT_FALSE(cwd_error) << cwd_error.message();
    const fs::path relative = "modworks data";
    const DataPaths paths = DataPathsFor(relative);
    const fs::path root = fs::absolute(dir / relative).lexically_normal();
    EXPECT_EQ(paths.Root, root);
    EXPECT_EQ(paths.LayoutIni, root / L"layout.ini");
    EXPECT_EQ(paths.WindowIni, root / L"window.ini");
    EXPECT_EQ(paths.LogFile, root / L"logs" / L"modworks.log");
    EXPECT_EQ(paths.SettingsFile, root / L"store" / L"settings.json");
    EXPECT_EQ(paths.Catalog, root / L"store" / L"catalog.sqlite");
    EXPECT_TRUE(IsInsideDataRoot(paths.Root, paths.LayoutIni));
    EXPECT_TRUE(IsInsideDataRoot(paths.Root, paths.LogFile));
    EXPECT_TRUE(IsInsideDataRoot(paths.Root, paths.Catalog));
    EXPECT_FALSE(IsInsideDataRoot(paths.Root, dir / L"catalog.sqlite"));
    EXPECT_FALSE(fs::exists(dir / "layout.ini"));
    EXPECT_FALSE(fs::exists(dir / "catalog.sqlite"));
}

TEST_F(StartupTest, UnicodeDataRootKeepsTheStoreBesideThatRoot) {
    const fs::path root = dir / L"modworks data \u043F\u0443\u0442\u044C";
    const fs::path chosen = ResolveDataRoot(root, L"C:\\modworks-decoy-not-opened");
    EXPECT_EQ(fs::absolute(chosen).lexically_normal(), fs::absolute(root).lexically_normal());
    const DataRootInspection inspection = InspectDataRoot(chosen);
    ASSERT_TRUE(inspection.Writable) << inspection.Message;
    EXPECT_FALSE(inspection.CatalogPresent);
    {
        const LocalModStore store(chosen);
        EXPECT_EQ(store.Root(), fs::absolute(root).lexically_normal());
        EXPECT_EQ(DataPathsFor(store.Root()).Catalog, store.Database());
        EXPECT_TRUE(IsInsideDataRoot(store.Root(), store.Database()));
        EXPECT_TRUE(store.Load().Sources.empty());
        EXPECT_TRUE(fs::is_regular_file(store.Database()));
    }
    EXPECT_FALSE(fs::exists(L"C:\\modworks-decoy-not-opened"));
    EXPECT_FALSE(fs::exists(dir / "catalog.sqlite"));
    EXPECT_FALSE(fs::exists(fs::current_path() / "layout.ini"));
}

TEST_F(StartupTest, GamePathUsesTheCommandLineThenTheSavedPath) {
    const fs::path saved = dir / "saved" / "pak01_dir.vpk";
    const fs::path command = dir / "manual" / "pak01_dir.vpk";
    const ResolvedGamePath detect = ResolveGamePath(std::nullopt, {});
    EXPECT_TRUE(detect.AutoDetect);
    EXPECT_EQ(detect.Source, GamePathSource::None);
    EXPECT_TRUE(detect.Path.empty());

    const ResolvedGamePath kept = ResolveGamePath(std::nullopt, saved);
    EXPECT_FALSE(kept.AutoDetect);
    EXPECT_EQ(kept.Source, GamePathSource::Saved);
    EXPECT_EQ(kept.Path, saved);

    const ResolvedGamePath manual = ResolveGamePath(command, saved);
    EXPECT_FALSE(manual.AutoDetect);
    EXPECT_EQ(manual.Source, GamePathSource::CommandLine);
    EXPECT_EQ(manual.Path, command);

    const ResolvedGamePath blank = ResolveGamePath(fs::path{}, saved);
    EXPECT_EQ(blank.Source, GamePathSource::Saved);
    EXPECT_FALSE(blank.AutoDetect);
}

TEST_F(StartupTest, CorruptSettingsDoNotReplaceTheCatalog) {
    const fs::path root = dir / "library";
    const fs::path vpk = dir / "mod.vpk";
    WriteArchive(vpk);
    std::vector<uint8_t> catalog;
    fs::path settings;
    {
        LocalModStore store(root);
        const fs::path cached = store.Import(vpk);
        ModelLibrary library;
        library.Sources.push_back(cached);
        library.GameArchive = dir / "missing-game" / "pak01_dir.vpk";
        store.Save(library);
        const ModelLibrary loaded = store.Load();
        ASSERT_EQ(loaded.Sources.size(), 1u);
        EXPECT_EQ(loaded.GameArchive, fs::absolute(library.GameArchive).lexically_normal());
        catalog = ReadBytes(store.Database());
        settings = DataPathsFor(store.Root()).SettingsFile;
    }
    ASSERT_FALSE(catalog.empty());
    const std::string corrupt = "{not json";
    WriteText(settings, corrupt);

    const SettingsInspection inspection = InspectLibrarySettings(settings.parent_path());
    EXPECT_EQ(inspection.State, SettingsState::Corrupt);
    EXPECT_TRUE(inspection.GameDirectory.empty());
    EXPECT_EQ(inspection.Message, kCorruptSettingsMessage);
    EXPECT_EQ(ReadText(settings), corrupt);
    EXPECT_EQ(ReadBytes(root / "store" / "catalog.sqlite"), catalog);

    const LibrarySettings ignored = LoadLibrarySettings(settings.parent_path());
    EXPECT_TRUE(ignored.GameDirectory.empty());
    EXPECT_EQ(ReadText(settings), corrupt);

    {
        const LocalModStore store(root);
        const ModelLibrary loaded = store.Load();
        ASSERT_EQ(loaded.Sources.size(), 1u);
        EXPECT_FALSE(loaded.GameArchive.empty());
        store.Save(loaded);
        EXPECT_EQ(store.Load().Sources.size(), 1u);
    }
    EXPECT_EQ(ReadText(settings), corrupt);
    EXPECT_GE(fs::file_size(root / "store" / "catalog.sqlite"), catalog.size());
}

TEST_F(StartupTest, ReadOnlyCatalogIsNotReplaced) {
    const fs::path root = dir / "library";
    const fs::path vpk = dir / "mod.vpk";
    WriteArchive(vpk);
    fs::path catalog_path;
    std::vector<uint8_t> catalog;
    {
        LocalModStore store(root);
        const fs::path cached = store.Import(vpk);
        ModelLibrary library;
        library.Sources.push_back(cached);
        store.Save(library);
        ASSERT_EQ(store.Load().Sources.size(), 1u);
        catalog_path = store.Database();
        catalog = ReadBytes(catalog_path);
    }
    ASSERT_NE(SetFileAttributesW(catalog_path.c_str(), FILE_ATTRIBUTE_READONLY), 0);
    const DataRootInspection inspection = InspectDataRoot(root);
    EXPECT_FALSE(inspection.Writable);
    EXPECT_TRUE(inspection.CatalogPresent);
    EXPECT_EQ(inspection.CatalogBytes, catalog.size());
    EXPECT_NE(inspection.Message.find("not replaced"), std::string::npos);
    EXPECT_EQ(ReadBytes(catalog_path), catalog);
    ASSERT_NE(SetFileAttributesW(catalog_path.c_str(), FILE_ATTRIBUTE_NORMAL), 0);

    WriteText(dir / "not-a-directory", "file");
    const DataRootInspection blocked = InspectDataRoot(dir / "not-a-directory");
    EXPECT_FALSE(blocked.Writable);
    EXPECT_NE(blocked.Message.find("not replaced"), std::string::npos);
    EXPECT_EQ(ReadText(dir / "not-a-directory"), "file");

    const DataRootInspection nested = InspectDataRoot(dir / "not-a-directory" / "child");
    EXPECT_FALSE(nested.Writable);
    EXPECT_FALSE(nested.Message.empty());
    EXPECT_FALSE(fs::exists(dir / "not-a-directory" / "child"));
}

TEST_F(StartupTest, ErrorLogIsUnderTheDataRootAndStartupCanStayQuiet) {
    const fs::path root = dir / "modworks data";
    const DataPaths paths = DataPathsFor(root);
    std::error_code cwd_error;
    fs::current_path(dir, cwd_error);
    ASSERT_FALSE(cwd_error) << cwd_error.message();
    EXPECT_TRUE(AppendErrorLog(paths.LogFile, {}));
    EXPECT_FALSE(fs::exists(paths.LogFile));
    EXPECT_FALSE(fs::exists(dir / "modworks.log"));

    const std::string details = DiagnosticText("Deadlock Modworks 0.1.0", "abc123", paths.Root, kCorruptSettingsMessage);
    EXPECT_NE(details.find("Deadlock Modworks 0.1.0"), std::string::npos);
    EXPECT_NE(details.find("abc123"), std::string::npos);
    EXPECT_NE(details.find("modworks data"), std::string::npos);
    EXPECT_NE(details.find(kCorruptSettingsMessage), std::string::npos);
    EXPECT_NE(DiagnosticText("Deadlock Modworks 0.1.0", "abc123", paths.Root, {}).find("No error is waiting."), std::string::npos);

    ASSERT_TRUE(AppendErrorLog(paths.LogFile, details));
    ASSERT_TRUE(AppendErrorLog(paths.LogFile, "second error"));
    const std::string log = ReadText(paths.LogFile);
    EXPECT_NE(log.find(kCorruptSettingsMessage), std::string::npos);
    EXPECT_NE(log.find("second error"), std::string::npos);
    EXPECT_TRUE(IsInsideDataRoot(paths.Root, paths.LogFile));
    EXPECT_FALSE(fs::exists(dir / "modworks.log"));
}

TEST(StartupMessages, QuickStartAndGameNeedsStaySpecific) {
    const std::span<const GuideStep> steps = QuickStartSteps();
    ASSERT_EQ(steps.size(), 8u);
    const char* titles[] = {"Extract", "Launch", "Connect", "Import", "Inspect", "Select", "Build", "Install or remove"};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        EXPECT_STREQ(steps[i].Title, titles[i]);
        EXPECT_NE(steps[i].Detail, nullptr);
        EXPECT_STRNE(steps[i].Detail, "");
    }
    EXPECT_NE(std::string(steps[6].Detail).find("does not install"), std::string::npos);
    EXPECT_NE(std::string(steps[7].Detail).find("does not install"), std::string::npos);
    const std::string needs = GameResourceSummary();
    EXPECT_NE(needs.find("without a connected game"), std::string::npos);
    EXPECT_NE(needs.find("base-game"), std::string::npos);
    EXPECT_NE(std::string(kAssemblyDoesNotInstall).find("does not guarantee"), std::string::npos);
    EXPECT_NE(std::string(kMissingVulkanLoaderMessage).find("vulkan-1.dll"), std::string::npos);
    EXPECT_NE(std::string(kMissingVulkanLoaderMessage).find("not opened"), std::string::npos);
    const fs::path game = "C:\\Games\\Deadlock\\game\\citadel\\pak01_dir.vpk";
    EXPECT_NE(MissingGameMessage(game).find("pak01_dir.vpk"), std::string::npos);
    EXPECT_NE(MissingGameMessage(game).find("not changed"), std::string::npos);
    EXPECT_EQ(ExportResultSummary(2, game), "Exported 2 files to " + Narrow(game.wstring()) + ".");
    EXPECT_EQ(AssembleResultSummary(game), "Assembled a pack at " + Narrow(game.wstring()) + ".");
}

}  // namespace
