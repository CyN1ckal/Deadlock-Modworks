#include "cancellation.hpp"
#include "command_line.hpp"
#include "content_hash.hpp"
#include "content_store.hpp"
#include "strings.hpp"
#include "vpk.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using skeleton_repair::BlobId;
using skeleton_repair::CancellationSource;
using skeleton_repair::CommandLine;
using skeleton_repair::ContentStore;
using skeleton_repair::Narrow;
using skeleton_repair::RepairChange;
using skeleton_repair::RepairModelNote;
using skeleton_repair::RepairRequest;
using skeleton_repair::Sha256;
using skeleton_repair::SnapshotId;
using skeleton_repair::VpkArchive;
using skeleton_repair::VpkChange;

class TempDir {
public:
    TempDir() {
        static int sequence = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path() / ("dmu-cli-" + std::to_string(ticks) + "-" + std::to_string(++sequence));
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

std::string Utf8(const fs::path& path) { return Narrow(path.wstring()); }

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output.good()) << path.string();
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(output.good()) << path.string();
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void Archive(const fs::path& path, const std::vector<VpkChange>& changes) {
    std::vector<uint8_t> seed;
    AppendU32(seed, 0x55AA1234u);
    AppendU32(seed, 2u);
    AppendU32(seed, 1u);
    AppendU32(seed, 0u);
    seed.resize(28, 0);
    seed.push_back(0);
    WriteBytes(path, seed);
    VpkArchive::Open(path).Write(path, changes);
}

struct CliOutput {
    int Code = 0;
    std::string Text;
};

CliOutput Execute(const std::vector<std::string>& args, skeleton_repair::CancellationToken token = {}) {
    CliOutput run;
    run.Code = CommandLine::Execute(args, [&](std::string_view line) {
        run.Text.append(line);
        run.Text.push_back('\n');
    }, token);
    return run;
}

BlobId IdOf(const std::vector<uint8_t>& bytes) {
    BlobId id;
    id.Bytes = Sha256(bytes);
    return id;
}

std::string ModelLine(const BlobId& blob, int packs, std::string_view path) {
    return blob.Hex().substr(0, 12) + "  " + std::to_string(packs) + " packs  " + std::string(path);
}

}  // namespace

TEST(CommandLineStore, HelpAndWrongArgumentsKeepExitCodes) {
    const CliOutput help = Execute({"--help"});
    EXPECT_EQ(help.Code, 0);
    EXPECT_NE(help.Text.find("--analyze <citadel-folder> <mod.vpk>"), std::string::npos);
    EXPECT_NE(help.Text.find("--update  <citadel-folder> <mod.vpk> <new-output.vpk>"), std::string::npos);
    EXPECT_NE(help.Text.find("--repair  <citadel-folder> <mod.vpk>"), std::string::npos);
    EXPECT_NE(help.Text.find("--store <dir> import <file.vpk>"), std::string::npos);
    EXPECT_EQ(Execute({"-h"}).Code, 0);

    EXPECT_EQ(Execute({}).Code, 2);
    EXPECT_EQ(Execute({"--analyze"}).Code, 2);
    EXPECT_EQ(Execute({"--update", "game", "mod.vpk"}).Code, 2);
    EXPECT_EQ(Execute({"--repair", "game"}).Code, 2);
    EXPECT_EQ(Execute({"--analyze", "game", "mod.vpk", "extra"}).Code, 2);
    EXPECT_EQ(Execute({"--store"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library", "import"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library", "models", "extra"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library", "duplicates", "extra"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library", "history"}).Code, 2);
    EXPECT_EQ(Execute({"--store", "library", "missing"}).Code, 2);

    TempDir dir;
    const CliOutput bad_hex = Execute({"--store", Utf8(dir.Path() / "store"), "history", "abcd"});
    EXPECT_EQ(bad_hex.Code, 2);
    EXPECT_NE(bad_hex.Text.find("Blob id must be 64 hex characters."), std::string::npos);
    EXPECT_FALSE(fs::exists(dir.Path() / "store"));
}

TEST(CommandLineStore, ImportReportsModelsAndNewBlobs) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "two.vpk";
    const std::vector<uint8_t> model_a{1, 2, 3, 4};
    const std::vector<uint8_t> model_b{5, 6, 7, 8};
    Archive(vpk, {VpkChange{"models/a.vmdl_c", model_a}, VpkChange{"models/b.vmdl_c", model_b}});

    const std::string store_arg = Utf8(store);
    const std::string vpk_arg = Utf8(vpk);
    const CliOutput first = Execute({"--store", store_arg, "import", vpk_arg});
    EXPECT_EQ(first.Code, 0);
    EXPECT_NE(first.Text.find("Imported 2 models, 2 new blobs.\n"), std::string::npos);

    std::string snapshot;
    {
        ContentStore opened = ContentStore::Open(store);
        const std::vector<skeleton_repair::PackRow> packs = opened.ListPacks();
        ASSERT_EQ(packs.size(), 1u);
        snapshot = packs[0].Snapshot.Value;
        EXPECT_EQ(packs[0].ModelCount, 2);
    }
    EXPECT_NE(first.Text.find(snapshot + "\n"), std::string::npos);

    const CliOutput second = Execute({"--store", store_arg, "import", vpk_arg});
    EXPECT_EQ(second.Code, 0);
    EXPECT_NE(second.Text.find(snapshot + "\n"), std::string::npos);
    EXPECT_NE(second.Text.find("Imported 2 models, 0 new blobs.\n"), std::string::npos);

    const CliOutput models = Execute({"--store", store_arg, "models"});
    EXPECT_EQ(models.Code, 0);
    const std::string line_a = ModelLine(IdOf(model_a), 1, "models/a.vmdl_c");
    const std::string line_b = ModelLine(IdOf(model_b), 1, "models/b.vmdl_c");
    EXPECT_NE(models.Text.find(line_a + "\n"), std::string::npos);
    EXPECT_NE(models.Text.find(line_b + "\n"), std::string::npos);
    EXPECT_EQ(models.Text, line_a + "\n" + line_b + "\n");

    const CliOutput duplicates = Execute({"--store", store_arg, "duplicates"});
    EXPECT_EQ(duplicates.Code, 0);
    EXPECT_TRUE(duplicates.Text.empty());
}

TEST(CommandLineStore, ImportCountsTextureBlobsSeparatelyFromModels) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    const fs::path vpk = dir.Path() / "pack.vpk";
    Archive(vpk, {
        VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1, 1, 1}},
        VpkChange{"models/b.vmdl_c", std::vector<uint8_t>{2, 2, 2}},
        VpkChange{"materials/t.vtex_c", std::vector<uint8_t>{9, 9, 9, 9}},
    });

    const CliOutput imported = Execute({"--store", Utf8(store), "import", Utf8(vpk)});
    EXPECT_EQ(imported.Code, 0);
    EXPECT_NE(imported.Text.find("Imported 2 models, 3 new blobs.\n"), std::string::npos);

    const CliOutput models = Execute({"--store", Utf8(store), "models"});
    EXPECT_EQ(models.Code, 0);
    EXPECT_EQ(models.Text.find("vtex_c"), std::string::npos);
}

TEST(CommandLineStore, DuplicatesUseTheModelLineAndStayEmptyAtExitZero) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    const std::vector<uint8_t> shared{3, 3, 3};
    const std::vector<uint8_t> once{4, 4, 4};
    const fs::path first_vpk = dir.Path() / "first.vpk";
    const fs::path second_vpk = dir.Path() / "second.vpk";
    Archive(first_vpk, {VpkChange{"models/shared.vmdl_c", shared}, VpkChange{"models/once.vmdl_c", once}});
    Archive(second_vpk, {VpkChange{"models/shared.vmdl_c", shared}});

    const std::string store_arg = Utf8(store);
    EXPECT_EQ(Execute({"--store", store_arg, "import", Utf8(first_vpk)}).Code, 0);
    const CliOutput empty = Execute({"--store", store_arg, "duplicates"});
    EXPECT_EQ(empty.Code, 0);
    EXPECT_TRUE(empty.Text.empty());

    EXPECT_EQ(Execute({"--store", store_arg, "import", Utf8(second_vpk)}).Code, 0);
    const CliOutput duplicates = Execute({"--store", store_arg, "duplicates"});
    EXPECT_EQ(duplicates.Code, 0);
    EXPECT_EQ(duplicates.Text, ModelLine(IdOf(shared), 2, "models/shared.vmdl_c") + "\n");
    EXPECT_EQ(duplicates.Text.find("models/once.vmdl_c"), std::string::npos);
}

TEST(CommandLineStore, HistoryAfterRepairPrintsMutatedOldestFirst) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    const std::vector<uint8_t> model_a{1, 1, 1};
    const std::vector<uint8_t> model_b{2, 2, 2};
    const std::vector<uint8_t> model_a2{1, 1, 2};
    const BlobId id_a = IdOf(model_a);
    const BlobId id_b = IdOf(model_b);
    const BlobId id_a2 = IdOf(model_a2);
    const fs::path vpk = dir.Path() / "mod.vpk";
    Archive(vpk, {VpkChange{"models/a.vmdl_c", model_a}, VpkChange{"models/b.vmdl_c", model_b}});

    {
        ContentStore opened = ContentStore::Open(store);
        const SnapshotId input = opened.ImportVpk(vpk);
        RepairRequest repair;
        repair.Input = input;
        repair.OutputPath = "D:/out/repaired.vpk";
        repair.Changes.push_back(RepairChange{"models/a.vmdl_c", "models/a.vmdl_c", model_a2});
        repair.Models.push_back(RepairModelNote{"models/a.vmdl_c", "Updated", "mutate-a", {}, {}, {}, std::nullopt});
        opened.RecordRepair(repair);
    }

    const std::string expected = id_a.Hex().substr(0, 12) + "  models/a.vmdl_c  imported\n"
        + id_a2.Hex().substr(0, 12) + "  models/a.vmdl_c  mutated\n";
    const CliOutput from_new = Execute({"--store", Utf8(store), "history", id_a2.Hex()});
    EXPECT_EQ(from_new.Code, 0);
    EXPECT_EQ(from_new.Text, expected);
    EXPECT_NE(from_new.Text.find(id_a.Hex().substr(0, 12)), std::string::npos);
    EXPECT_NE(from_new.Text.find(id_a2.Hex().substr(0, 12)), std::string::npos);
    EXPECT_NE(from_new.Text.find("mutated"), std::string::npos);

    const CliOutput from_old = Execute({"--store", Utf8(store), "history", id_a.Hex()});
    EXPECT_EQ(from_old.Code, 0);
    EXPECT_EQ(from_old.Text, expected);

    const CliOutput untouched = Execute({"--store", Utf8(store), "history", id_b.Hex()});
    EXPECT_EQ(untouched.Code, 0);
    EXPECT_EQ(untouched.Text, id_b.Hex().substr(0, 12) + "  models/b.vmdl_c  imported\n");
    EXPECT_EQ(untouched.Text.find("mutated"), std::string::npos);
}

TEST(CommandLineStore, StoreCommandsReportFailuresWithoutAWindow) {
    TempDir dir;
    const fs::path store = dir.Path() / "store";
    const CliOutput missing = Execute({"--store", Utf8(store), "import", Utf8(dir.Path() / "missing.vpk")});
    EXPECT_EQ(missing.Code, 2);
    EXPECT_NE(missing.Text.find("Could not find the mod VPK."), std::string::npos);
    EXPECT_EQ(missing.Text.find("Imported "), std::string::npos);

    const fs::path chunk = dir.Path() / "pack_000.vpk";
    Archive(chunk, {VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1}}});
    const CliOutput rejected = Execute({"--store", Utf8(store), "import", Utf8(chunk)});
    EXPECT_EQ(rejected.Code, 2);
    EXPECT_NE(rejected.Text.find("Import the directory VPK, not a chunk file."), std::string::npos);

    CancellationSource source;
    source.Cancel();
    const fs::path vpk = dir.Path() / "mod.vpk";
    Archive(vpk, {VpkChange{"models/a.vmdl_c", std::vector<uint8_t>{1, 2}}});
    const CliOutput cancelled = Execute({"--store", Utf8(dir.Path() / "other-store"), "import", Utf8(vpk)}, source.Token());
    EXPECT_EQ(cancelled.Code, 2);
    EXPECT_NE(cancelled.Text.find("Cancelled."), std::string::npos);
    EXPECT_FALSE(fs::exists(dir.Path() / "other-store"));
}
