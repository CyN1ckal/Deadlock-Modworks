#include "addons_deployer.hpp"
#include "error.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace skeleton_repair;

struct TempFolder {
    fs::path Path = fs::temp_directory_path() / ("addons-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    TempFolder() { fs::remove_all(Path); fs::create_directories(Path); }
    ~TempFolder() { std::error_code error; fs::remove_all(Path, error); }
};

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

void Write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary | std::ios::trunc) << text; }

// Library copies with distinct contents. The deployer copies them as they are, so they need not be VPKs.
struct Library {
    fs::path Root;
    std::map<std::string, DeployMod> Mods;
    explicit Library(fs::path root) : Root(std::move(root)) { fs::create_directories(Root); }
    const DeployMod& Add(const std::string& id) {
        const auto path = Root / (id + ".vpk");
        Write(path, "content of " + id);
        return Mods[id] = DeployMod{id, "Mod " + id, path};
    }
    std::vector<DeployMod> List(std::initializer_list<const char*> ids) {
        std::vector<DeployMod> result;
        for (const auto* id : ids) result.push_back(Mods.at(id));
        return result;
    }
};

std::map<std::string, std::string> Contents(const fs::path& addons) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::directory_iterator(addons))
        if (entry.path().filename() != kAddonsManifestName) result[entry.path().filename().string()] = Read(entry.path());
    return result;
}

TEST(AddonSlots, NamesAndParsesSlots) {
    EXPECT_EQ(AddonSlotName(1), "pak01_dir.vpk");
    EXPECT_EQ(AddonSlotName(42), "pak42_dir.vpk");
    EXPECT_EQ(AddonSlotOf("pak07_dir.vpk"), 7);
    EXPECT_EQ(AddonSlotOf("PAK07_DIR.VPK"), 7);
    EXPECT_EQ(AddonSlotOf("pak07_000.vpk"), 7);
    EXPECT_FALSE(AddonSlotOf("pak00_dir.vpk"));
    EXPECT_FALSE(AddonSlotOf("pak7_dir.vpk"));
    EXPECT_FALSE(AddonSlotOf("619503_pak13_dir.vpk"));
    EXPECT_FALSE(AddonSlotOf("graves_fn.vpk"));
    EXPECT_FALSE(AddonSlotOf("pak07_dir.vpk.disabled"));
}

TEST(AddonSlots, AssignsLowestFreeSlotsAroundForeignFiles) {
    EXPECT_EQ(AssignAddonSlots({}, 3), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(AssignAddonSlots({1, 3}, 3), (std::vector<int>{2, 4, 5}));
    std::set<int> full;
    for (int slot = 1; slot <= 98; ++slot) full.insert(slot);
    EXPECT_EQ(AssignAddonSlots(full, 1), (std::vector<int>{99}));
    EXPECT_THROW(AssignAddonSlots(full, 2), InvalidOperation);
}

TEST(AddonsDeploy, CopiesEnabledModsInPriorityOrder) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    const auto result = DeployAddons(addons, library.List({"b", "a"}));
    EXPECT_EQ(result.Copied, 2u);
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "content of b"}, {"pak02_dir.vpk", "content of a"}}));
    const auto scan = ScanAddons(addons);
    ASSERT_EQ(scan.Managed.size(), 2u);
    EXPECT_TRUE(scan.Foreign.empty());
}

TEST(AddonsDeploy, KeepsMovesAndRemovesItsOwnFiles) {
    TempFolder temp;
    Library library(temp.Path / "library");
    for (const auto* id : {"a", "b", "c"}) library.Add(id);
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a", "b", "c"}));

    const auto swapped = DeployAddons(addons, library.List({"c", "b"}));
    EXPECT_EQ(swapped.Copied, 0u);
    EXPECT_EQ(swapped.Kept, 1u);
    EXPECT_EQ(swapped.Moved, 1u);
    EXPECT_EQ(swapped.Removed, 1u);
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "content of c"}, {"pak02_dir.vpk", "content of b"}}));

    const auto none = DeployAddons(addons, {});
    EXPECT_EQ(none.Removed, 2u);
    EXPECT_TRUE(Contents(addons).empty());
    EXPECT_TRUE(ScanAddons(addons).Managed.empty());
}

TEST(AddonsDeploy, NeverTouchesFilesItDidNotWrite) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    fs::create_directories(addons);
    Write(addons / "pak01_dir.vpk", "someone else's mod");
    Write(addons / "pak03_dir.vpk", "chunked");
    Write(addons / "pak03_000.vpk", "chunk");
    Write(addons / "619503_pak13_dir.vpk", "disabled by another manager");
    Write(addons / "readme.txt", "notes");

    DeployAddons(addons, library.List({"a", "b"}));
    auto contents = Contents(addons);
    EXPECT_EQ(contents["pak01_dir.vpk"], "someone else's mod");
    EXPECT_EQ(contents["pak02_dir.vpk"], "content of a");
    EXPECT_EQ(contents["pak03_dir.vpk"], "chunked");
    EXPECT_EQ(contents["pak04_dir.vpk"], "content of b");
    EXPECT_EQ(contents.size(), 7u);

    const auto scan = ScanAddons(addons);
    EXPECT_EQ(scan.Foreign, (std::set<int>{1, 3}));
    DeployAddons(addons, {});
    contents = Contents(addons);
    EXPECT_EQ(contents.size(), 5u);
    EXPECT_EQ(contents["pak01_dir.vpk"], "someone else's mod");
}

TEST(AddonsDeploy, LeavesFilesChangedByHandAlone) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a"}));
    Write(addons / "pak01_dir.vpk", "replaced by the user");

    const auto scan = ScanAddons(addons);
    EXPECT_TRUE(scan.Managed.empty());
    EXPECT_EQ(scan.Foreign, (std::set<int>{1}));
    ASSERT_EQ(scan.Notes.size(), 1u);

    const auto result = DeployAddons(addons, library.List({"a"}));
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "replaced by the user"}, {"pak02_dir.vpk", "content of a"}}));
    EXPECT_EQ(result.Deployed.size(), 1u);
}

TEST(AddonsDeploy, ForgetsFilesRemovedByHand) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a"}));
    fs::remove(addons / "pak01_dir.vpk");
    EXPECT_TRUE(ScanAddons(addons).Managed.empty());
    const auto result = DeployAddons(addons, library.List({"a"}));
    EXPECT_EQ(result.Copied, 1u);
}

TEST(AddonsDeploy, RemovesLeftoverTemporaryFiles) {
    TempFolder temp;
    const auto addons = temp.Path / "addons";
    fs::create_directories(addons);
    Write(addons / ".modworks-1-1.tmp", "partial");
    ScanAddons(addons);
    EXPECT_FALSE(fs::exists(addons / ".modworks-1-1.tmp"));
}

TEST(AddonsDeploy, PutsAParkedSlotFileBack) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a"}));
    fs::rename(addons / "pak01_dir.vpk", addons / ".modworks-aside-1.tmp");
    const auto scan = ScanAddons(addons);
    EXPECT_EQ(Read(addons / "pak01_dir.vpk"), "content of a");
    ASSERT_EQ(scan.Managed.size(), 1u);
    EXPECT_EQ(scan.Managed[0].ContentId, "a");
    EXPECT_FALSE(fs::exists(addons / ".modworks-aside-1.tmp"));
}

TEST(AddonsDeploy, TreatsADamagedRecordAsForeign) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a"}));
    Write(addons / kAddonsManifestName, "{ broken");
    const auto scan = ScanAddons(addons);
    EXPECT_TRUE(scan.Managed.empty());
    EXPECT_EQ(scan.Foreign, (std::set<int>{1}));
    EXPECT_FALSE(scan.Notes.empty());
}

TEST(AddonsDeploy, FailedCopyPutsMovedModsBack) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a", "b"}));
    auto mods = library.List({"b", "a"});
    mods.push_back(DeployMod{"c", "Mod c", temp.Path / "missing.vpk"});
    EXPECT_THROW(DeployAddons(addons, mods), IoError);
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "content of a"}, {"pak02_dir.vpk", "content of b"}}));
    const auto scan = ScanAddons(addons);
    ASSERT_EQ(scan.Managed.size(), 2u);
    EXPECT_EQ(scan.Managed[0].ContentId, "a");
    EXPECT_EQ(scan.Managed[1].ContentId, "b");
}

TEST(AddonsDeploy, FailedCopyPutsRemovedModsBack) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a", "b"}));
    EXPECT_THROW(DeployAddons(addons, {DeployMod{"c", "Mod c", temp.Path / "missing.vpk"}}), IoError);
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "content of a"}, {"pak02_dir.vpk", "content of b"}}));
    EXPECT_EQ(ScanAddons(addons).Managed.size(), 2u);
}

TEST(AddonsDeploy, CancelDuringCopyPutsMovedModsBack) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a", "b"}));
    Write(temp.Path / "c.vpk", std::string(1 << 20, 'c'));
    auto mods = library.List({"b", "a"});
    mods.push_back(DeployMod{"c", "Mod c", temp.Path / "c.vpk"});
    CancellationSource cancel;
    EXPECT_THROW(DeployAddons(addons, mods, cancel.Token(), [&](uint64_t copied, uint64_t) { if (copied > 0) cancel.Cancel(); }), OperationCancelled);
    EXPECT_EQ(Contents(addons), (std::map<std::string, std::string>{{"pak01_dir.vpk", "content of a"}, {"pak02_dir.vpk", "content of b"}}));
    EXPECT_EQ(ScanAddons(addons).Managed.size(), 2u);
}

TEST(AddonsDeploy, CancellingKeepsTheRecordOfWhatIsInPlace) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    library.Add("b");
    const auto addons = temp.Path / "addons";
    DeployAddons(addons, library.List({"a"}));
    CancellationSource cancel;
    cancel.Cancel();
    EXPECT_THROW(DeployAddons(addons, library.List({"b"}), cancel.Token()), OperationCancelled);
    const auto scan = ScanAddons(addons);
    ASSERT_EQ(scan.Managed.size(), 1u);
    EXPECT_EQ(scan.Managed[0].ContentId, "a");
}

TEST(AddonsDeploy, RefusesTheSameModTwice) {
    TempFolder temp;
    Library library(temp.Path / "library");
    library.Add("a");
    EXPECT_THROW(DeployAddons(temp.Path / "addons", library.List({"a", "a"})), InvalidOperation);
}

void MakeVpk(const fs::path& path, const std::vector<std::string>& files) {
    std::vector<VpkNewFile> entries;
    for (const auto& file : files) {
        const std::vector<uint8_t> data(file.begin(), file.end());
        entries.push_back({file, VpkArchive::Crc(data), static_cast<uint32_t>(data.size()),
            [data](const std::function<void(const uint8_t*, size_t)>& sink) { sink(data.data(), data.size()); }});
    }
    VpkArchive::Create(path, entries);
}

TEST(ModConflicts, ReportsSharedFilesWithTheWinnerFirst) {
    TempFolder temp;
    MakeVpk(temp.Path / "a.vpk", {"models/heroes/haze/haze.vmdl_c", "panorama/a.vtex_c"});
    MakeVpk(temp.Path / "b.vpk", {"models/heroes/haze/haze.vmdl_c", "panorama/b.vtex_c"});
    MakeVpk(temp.Path / "c.vpk", {"panorama/c.vtex_c"});
    const auto conflicts = FindModConflicts({{"a", "A", temp.Path / "a.vpk"}, {"b", "B", temp.Path / "b.vpk"},
        {"c", "C", temp.Path / "c.vpk"}, {"missing", "M", temp.Path / "missing.vpk"}});
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].Winner, "a");
    EXPECT_EQ(conflicts[0].Loser, "b");
    EXPECT_EQ(conflicts[0].Files, 1u);
    EXPECT_EQ(conflicts[0].Examples, (std::vector<std::string>{"models/heroes/haze/haze.vmdl_c"}));
}

constexpr const char* kGameInfo =
    "\"GameInfo\"\r\n{\r\n\tFileSystem\r\n\t{\r\n\t\tSearchPaths\r\n\t\t{\r\n"
    "\t\t\tGame_LowViolence\tcitadel_lv\r\n\t\t\tMod\t\t\t\t\tcitadel\r\n"
    "\t\t\tGame\t\t\t\tcitadel\r\n\t\t\tGame\t\t\t\tcore\r\n\t\t}\r\n\t}\r\n}\r\n";

TEST(GameInfo, DetectsTheAddonsSearchPath) {
    EXPECT_FALSE(GameInfoMountsAddons(kGameInfo));
    EXPECT_TRUE(GameInfoMountsAddons("SearchPaths\n{\n  Game \"citadel/addons\"\n  Game citadel\n}\n"));
    EXPECT_TRUE(GameInfoMountsAddons("SearchPaths {\n  game citadel\\addons\\\n}\n"));
    EXPECT_FALSE(GameInfoMountsAddons("SearchPaths\n{\n  // Game citadel/addons\n  Game citadel\n}\n"));
    EXPECT_FALSE(GameInfoMountsAddons("Other\n{\n  Game citadel/addons\n}\n"));
}

TEST(GameInfo, AddsTheSearchPathBeforeTheGameFolder) {
    const auto patched = AddAddonsToGameInfo(kGameInfo);
    EXPECT_TRUE(GameInfoMountsAddons(patched));
    const auto addons = patched.find("\t\t\tGame\t\t\t\tcitadel/addons\r\n");
    ASSERT_NE(addons, std::string::npos);
    EXPECT_LT(addons, patched.find("\t\t\tGame\t\t\t\tcitadel\r\n"));
    EXPECT_EQ(patched.size(), std::string(kGameInfo).size() + std::string("\t\t\tGame\t\t\t\tcitadel/addons\r\n").size());
    EXPECT_EQ(AddAddonsToGameInfo(patched), patched);
    EXPECT_THROW(AddAddonsToGameInfo("\"GameInfo\" { }"), InvalidData);
}

TEST(GameInfo, PatchesTheFileAndKeepsABackup) {
    TempFolder temp;
    const auto citadel = temp.Path / "game" / "citadel";
    fs::create_directories(citadel);
    Write(citadel / "gameinfo.gi", kGameInfo);
    Write(citadel / "pak01_dir.vpk", "game");
    EXPECT_EQ(CitadelFolder(citadel / "pak01_dir.vpk"), citadel);
    EXPECT_EQ(CitadelFolder(temp.Path / "game"), citadel);
    EXPECT_FALSE(CitadelFolder(temp.Path / "x" / "y"));
    PatchGameInfo(citadel);
    EXPECT_TRUE(GameInfoMountsAddons(Read(citadel / "gameinfo.gi")));
    EXPECT_EQ(Read(citadel / "gameinfo.gi.modworks.bak"), kGameInfo);
}

}  // namespace
