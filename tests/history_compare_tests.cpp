#include "history_compare.hpp"

#include "content_store.hpp"
#include "strings.hpp"
#include "updater.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using skeleton_repair::BlobId;
using skeleton_repair::ContentStore;
using skeleton_repair::DirectHistoryLink;
using skeleton_repair::History;
using skeleton_repair::HistoryCompareCaption;
using skeleton_repair::HistoryLink;
using skeleton_repair::HistoryVersion;
using skeleton_repair::kNoRecordedTransform;
using skeleton_repair::kRepairNeedsGameFolder;
using skeleton_repair::kRepairSourceMissing;
using skeleton_repair::LibrarySettings;
using skeleton_repair::LoadLibrarySettings;
using skeleton_repair::ModelOutcome;
using skeleton_repair::Narrow;
using skeleton_repair::ObservationFileExists;
using skeleton_repair::RepairFailureText;
using skeleton_repair::RepairGate;
using skeleton_repair::RepairGateFor;
using skeleton_repair::SaveLibrarySettings;
using skeleton_repair::SnapshotId;
using skeleton_repair::UpdateReport;
using skeleton_repair::HeroModelUpdater;
using skeleton_repair::VpkArchive;
using skeleton_repair::VpkChange;

class TempDir {
public:
    TempDir() {
        static int sequence = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("dmu-cmp-" + std::to_string(ticks) + "-" + std::to_string(++sequence));
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

BlobId Blob(uint8_t fill) {
    BlobId id;
    id.Bytes.fill(fill);
    return id;
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good()) << path.string();
    output << text;
    ASSERT_TRUE(output.good()) << path.string();
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.good()) << path.string();
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void ArchiveWith(const fs::path& path, const std::vector<VpkChange>& changes) {
    std::vector<uint8_t> bytes;
    AppendU32(bytes, 0x55AA1234u);
    AppendU32(bytes, 2u);
    AppendU32(bytes, 1u);
    AppendU32(bytes, 0u);
    bytes.resize(28, 0);
    bytes.push_back(0);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good()) << path.string();
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good()) << path.string();
    output.close();
    VpkArchive::Open(path).Write(path, changes);
}

std::string AbsoluteUtf8(const fs::path& path) { return Narrow(fs::absolute(path).lexically_normal().wstring()); }

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.good()) << path.string();
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

TEST(HistoryCompare, CaptionIncludesTheMutatedRelationAndBothPrefixes) {
    History history;
    HistoryVersion original;
    original.Blob = Blob(0x11);
    HistoryVersion mutated;
    mutated.Blob = Blob(0x22);
    HistoryLink link;
    link.From = original.Blob;
    link.To = mutated.Blob;
    link.Relation = "mutated";
    link.Detail = "mutate-a";
    history.Versions = {original, mutated};
    history.Links = {link};

    const std::string caption = HistoryCompareCaption(history, mutated.Blob, original.Blob);
    EXPECT_EQ(caption, "111111111111  mutated  222222222222");
    EXPECT_EQ(caption, HistoryCompareCaption(history, original.Blob, mutated.Blob));
    EXPECT_EQ(caption.find(link.Detail), std::string::npos);
    ASSERT_NE(DirectHistoryLink(history, original.Blob, mutated.Blob), nullptr);
    EXPECT_EQ(DirectHistoryLink(history, original.Blob, mutated.Blob)->Relation, "mutated");

    link.Relation = "renamed+mutated";
    history.Links = {link};
    EXPECT_EQ(HistoryCompareCaption(history, original.Blob, mutated.Blob), "111111111111  renamed+mutated  222222222222");
}

TEST(HistoryCompare, CaptionUsesTheNoLinkSentenceWhenVersionsAreNotDirectlyLinked) {
    History history;
    HistoryVersion first;
    first.Blob = Blob(0x11);
    HistoryVersion second;
    second.Blob = Blob(0x22);
    HistoryVersion other;
    other.Blob = Blob(0x33);
    history.Versions = {first, second, other};

    EXPECT_EQ(HistoryCompareCaption(history, first.Blob, second.Blob), kNoRecordedTransform);
    EXPECT_EQ(kNoRecordedTransform, std::string("No recorded transform between these versions."));
    EXPECT_EQ(DirectHistoryLink(history, first.Blob, second.Blob), nullptr);

    HistoryLink side;
    side.From = first.Blob;
    side.To = other.Blob;
    side.Relation = "mutated";
    history.Links = {side};
    EXPECT_EQ(HistoryCompareCaption(history, first.Blob, second.Blob), kNoRecordedTransform);
    EXPECT_EQ(HistoryCompareCaption(history, second.Blob, other.Blob), kNoRecordedTransform);
    EXPECT_EQ(HistoryCompareCaption(history, first.Blob, first.Blob), kNoRecordedTransform);

    HistoryLink renamed;
    renamed.From = first.Blob;
    renamed.To = first.Blob;
    renamed.Relation = "renamed";
    history.Links.push_back(renamed);
    EXPECT_EQ(HistoryCompareCaption(history, first.Blob, second.Blob), kNoRecordedTransform);
}

TEST(HistoryCompare, RepairGateMessagesAreExact) {
    const RepairGate missing = RepairGateFor(false, false);
    EXPECT_FALSE(missing.Enabled);
    EXPECT_STREQ(missing.Message, kRepairSourceMissing);
    EXPECT_STREQ(missing.Message, "The imported VPK is no longer at its original path.");

    const RepairGate needs_game = RepairGateFor(true, false);
    EXPECT_FALSE(needs_game.Enabled);
    EXPECT_STREQ(needs_game.Message, kRepairNeedsGameFolder);
    EXPECT_STREQ(needs_game.Message, "Set the game folder before repairing.");

    const RepairGate ready = RepairGateFor(true, true);
    EXPECT_TRUE(ready.Enabled);
    EXPECT_EQ(ready.Message, nullptr);
}

TEST(HistoryCompare, ObservationFileExistsChecksTheRecordedPath) {
    EXPECT_FALSE(ObservationFileExists(""));
    EXPECT_FALSE(ObservationFileExists("C:\\missing\\deadlock-model-updater-no-such-file.vpk"));

    TempDir dir;
    const fs::path file = dir.Path() / "mod.vpk";
    WriteText(file, "vpk");
    const std::string path = AbsoluteUtf8(file);
    EXPECT_TRUE(ObservationFileExists(path));
    ASSERT_TRUE(fs::remove(file));
    EXPECT_FALSE(ObservationFileExists(path));
}

TEST(HistoryCompare, SettingsFileRoundTripsAndCorruptFilesStayEmpty) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    EXPECT_NO_THROW({
        EXPECT_TRUE(LoadLibrarySettings(store).GameDirectory.empty());
        EXPECT_FALSE(fs::exists(store / "settings.json"));
    });

    LibrarySettings settings;
    settings.GameDirectory = "C:\\Games\\citadel";
    SaveLibrarySettings(store, settings);
    EXPECT_EQ(ReadText(store / "settings.json"), "{\"gameDirectory\":\"C:\\\\Games\\\\citadel\"}");
    EXPECT_EQ(LoadLibrarySettings(store).GameDirectory, settings.GameDirectory);

    const std::string corrupt[] = {"not json", "{", "[]", "{\"gameDirectory\":12}", ""};
    for (const std::string& text : corrupt) {
        WriteText(store / "settings.json", text);
        LibrarySettings loaded;
        EXPECT_NO_THROW(loaded = LoadLibrarySettings(store));
        EXPECT_TRUE(loaded.GameDirectory.empty()) << text;
    }
}

TEST(HistoryCompare, PackObservationPathIsTheImportedFile) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path mod = dir.Path() / "mod.vpk";
    ArchiveWith(mod, {VpkChange{"models/props/crate.vmdl_c", std::vector<uint8_t>{1, 2, 3, 4}}});

    ContentStore store = ContentStore::Open(store_path);
    store.ImportVpk(mod);
    ASSERT_EQ(store.ListPacks().size(), 1u);
    const std::string recorded = store.ListPacks()[0].ObservationPath;
    EXPECT_EQ(recorded, AbsoluteUtf8(mod));
    EXPECT_TRUE(ObservationFileExists(recorded));
    ASSERT_TRUE(fs::remove(mod));
    EXPECT_EQ(store.ListPacks()[0].ObservationPath, recorded);
    EXPECT_FALSE(ObservationFileExists(recorded));
}

TEST(HistoryCompare, FailedRepairKeepsTheImportedSnapshot) {
    TempDir dir;
    const fs::path store_path = dir.Path() / "store";
    const fs::path game_dir = dir.Path() / "game";
    const fs::path mod = dir.Path() / "mods" / "mod.vpk";
    fs::create_directories(game_dir);
    fs::create_directories(mod.parent_path());
    const std::vector<uint8_t> payload{9, 8, 7, 6};
    ArchiveWith(game_dir / "pak01_dir.vpk", {VpkChange{"models/props/crate.vmdl_c", payload}});
    ArchiveWith(mod, {VpkChange{"models/props/crate.vmdl_c", payload}});

    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId input = store.ImportVpk(mod);
    ASSERT_EQ(store.ListModels().size(), 1u);
    const BlobId blob = store.ListModels()[0].Blob;
    const std::vector<uint8_t> stored = store.ReadBlob(blob);
    const std::vector<uint8_t> original_file = ReadFile(mod);

    const UpdateReport report = HeroModelUpdater::Update(Narrow(game_dir.wstring()), Narrow(mod.wstring()), std::nullopt, true, true,
        {}, {}, false, &store);
    EXPECT_FALSE(report.WroteFile);
    EXPECT_TRUE(report.HasBlockers());
    const std::string error = RepairFailureText(report);
    EXPECT_NE(error.find("No supported hero models"), std::string::npos);

    ASSERT_EQ(store.ListPacks().size(), 1u);
    EXPECT_EQ(store.ListPacks()[0].Snapshot.Value, input.Value);
    EXPECT_EQ(store.ReadBlob(blob), stored);
    const History history = store.HistoryOf(blob);
    ASSERT_EQ(history.Versions.size(), 1u);
    EXPECT_EQ(history.Versions[0].Blob.Bytes, blob.Bytes);
    EXPECT_TRUE(history.Links.empty());
    EXPECT_EQ(ReadFile(mod), original_file);
}

}  // namespace
