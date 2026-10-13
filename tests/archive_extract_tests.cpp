#include "archive_extract.hpp"
#include "error.hpp"

#include <gtest/gtest.h>

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skeleton_repair;

struct TempFolder {
    fs::path Path = fs::temp_directory_path() / ("extract-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    TempFolder() { fs::remove_all(Path); fs::create_directories(Path); }
    ~TempFolder() { std::error_code error; fs::remove_all(Path, error); }
};

using Entries = std::vector<std::pair<std::string, std::string>>;

// Writes a zip (or, with `seven_zip`, a 7z) archive whose entries are written exactly as named, even unsafe ones.
void MakeArchive(const fs::path& path, const Entries& entries, bool seven_zip = false) {
    archive* writer = archive_write_new();
    if (seven_zip) archive_write_set_format_7zip(writer);
    else archive_write_set_format_zip(writer);
    ASSERT_EQ(archive_write_open_filename_w(writer, path.c_str()), ARCHIVE_OK);
    for (const auto& [name, data] : entries) {
        archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, static_cast<la_int64_t>(data.size()));
        ASSERT_EQ(archive_write_header(writer, entry), ARCHIVE_OK);
        archive_write_data(writer, data.data(), data.size());
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

TEST(ArchiveExtract, RecognizesArchiveExtensions) {
    EXPECT_TRUE(IsModArchive("mod.zip"));
    EXPECT_TRUE(IsModArchive("mod.7Z"));
    EXPECT_TRUE(IsModArchive("mod.rar"));
    EXPECT_FALSE(IsModArchive("mod.vpk"));
    EXPECT_FALSE(IsModArchive("mod.zip.txt"));
}

TEST(ArchiveExtract, ExtractsOnlyModVpks) {
    TempFolder temp;
    MakeArchive(temp.Path / "mod.zip", {{"haze/pak04_dir.vpk", "haze"}, {"readme.txt", "hi"}, {"preview.png", "png"}, {"top.vpk", "top"}});
    const auto result = ExtractModArchive(temp.Path / "mod.zip", temp.Path / "out");
    ASSERT_EQ(result.Archives.size(), 2u);
    EXPECT_EQ(result.SkippedEntries, 2u);
    EXPECT_EQ(Read(result.Archives[0]), "haze");
    EXPECT_EQ(result.Archives[0].filename(), "pak04_dir.vpk");
    EXPECT_EQ(Read(result.Archives[1]), "top");
}

TEST(ArchiveExtract, ReadsSevenZip) {
    TempFolder temp;
    MakeArchive(temp.Path / "mod.7z", {{"a/b/mod.vpk", "seven"}}, true);
    const auto result = ExtractModArchive(temp.Path / "mod.7z", temp.Path / "out");
    ASSERT_EQ(result.Archives.size(), 1u);
    EXPECT_EQ(Read(result.Archives[0]), "seven");
}

TEST(ArchiveExtract, EntryPathsCannotLeaveTheDestination) {
    TempFolder temp;
    MakeArchive(temp.Path / "evil.zip", {{"../../escape.vpk", "x"}, {"C:/Windows/evil.vpk", "y"}, {"..\\back.vpk", "z"}});
    const auto out = temp.Path / "nested" / "out";
    const auto result = ExtractModArchive(temp.Path / "evil.zip", out);
    EXPECT_EQ(result.Archives.size(), 3u);
    for (const auto& file : result.Archives) {
        const auto relative = fs::relative(file, out);
        EXPECT_FALSE(relative.empty());
        EXPECT_NE(relative.begin()->string(), "..");
    }
    EXPECT_FALSE(fs::exists(temp.Path / "escape.vpk"));
    EXPECT_FALSE(fs::exists(temp.Path / "nested" / "escape.vpk"));
    EXPECT_FALSE(fs::exists(temp.Path / "back.vpk"));
}

TEST(ArchiveExtract, KeepsChunksBesideTheirDirectoryFile) {
    TempFolder temp;
    MakeArchive(temp.Path / "mod.zip", {{"big/pak01_dir.vpk", "dir"}, {"big/pak01_000.vpk", "chunk"}, {"other/pak01_dir.vpk", "single"}});
    const auto result = ExtractModArchive(temp.Path / "mod.zip", temp.Path / "out");
    ASSERT_EQ(result.Archives.size(), 2u);
    // A chunked pak01 would look like the game archive to the library, so it is renamed together with its chunks.
    EXPECT_EQ(result.Archives[0].filename(), "mod01_dir.vpk");
    EXPECT_EQ(Read(result.Archives[0].parent_path() / "mod01_000.vpk"), "chunk");
    EXPECT_EQ(result.Archives[1].filename(), "pak01_dir.vpk");
    EXPECT_NE(result.Archives[0].parent_path(), result.Archives[1].parent_path());
}

TEST(ArchiveExtract, RefusesArchivesWithoutMods) {
    TempFolder temp;
    MakeArchive(temp.Path / "empty.zip", {{"readme.txt", "hi"}, {"nested.zip", "zip"}});
    EXPECT_THROW(ExtractModArchive(temp.Path / "empty.zip", temp.Path / "out"), InvalidData);
}

TEST(ArchiveExtract, RefusesDamagedArchives) {
    TempFolder temp;
    std::ofstream(temp.Path / "bad.zip", std::ios::binary) << "this is not an archive";
    EXPECT_THROW(ExtractModArchive(temp.Path / "bad.zip", temp.Path / "out"), InvalidData);
}

TEST(ArchiveExtract, EnforcesSizeLimits) {
    TempFolder temp;
    MakeArchive(temp.Path / "big.zip", {{"a.vpk", std::string(4096, 'a')}});
    ExtractLimits limits;
    limits.MaxEntryBytes = 1024;
    EXPECT_THROW(ExtractModArchive(temp.Path / "big.zip", temp.Path / "out", {}, {}, limits), InvalidData);
}

TEST(ArchiveExtract, RequiresAnEmptyDestination) {
    TempFolder temp;
    MakeArchive(temp.Path / "mod.zip", {{"a.vpk", "a"}});
    fs::create_directories(temp.Path / "out");
    std::ofstream(temp.Path / "out" / "x") << "x";
    EXPECT_THROW(ExtractModArchive(temp.Path / "mod.zip", temp.Path / "out"), InvalidOperation);
}

TEST(ArchiveExtract, StopsWhenCancelled) {
    TempFolder temp;
    MakeArchive(temp.Path / "mod.zip", {{"a.vpk", "a"}});
    CancellationSource cancel;
    cancel.Cancel();
    EXPECT_THROW(ExtractModArchive(temp.Path / "mod.zip", temp.Path / "out", cancel.Token()), OperationCancelled);
}

}  // namespace
