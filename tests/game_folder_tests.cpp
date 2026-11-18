#include "game_folder.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using skeleton_repair::GameBuildId;
using skeleton_repair::ParseClientVersion;
using skeleton_repair::ParseSteamLibraryPaths;
namespace fs = std::filesystem;

TEST(GameFolderTests, ReadsTheClientVersionOfSteamInf) {
    EXPECT_EQ(ParseClientVersion("ClientVersion=6011\r\nServerVersion=6011\r\nPatchVersion=1.0.0.0\r\n"), "6011");
    EXPECT_EQ(ParseClientVersion("ProductName=citadel\nClientVersion = 5932 \n"), "5932");
    EXPECT_EQ(ParseClientVersion("ServerVersion=6011\n"), std::nullopt);
    EXPECT_EQ(ParseClientVersion("ClientVersion=\n"), std::nullopt);
}

TEST(GameFolderTests, BuildIdFollowsSteamInfThenTheArchive) {
    const fs::path folder = fs::temp_directory_path() / ("game-build-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    fs::remove_all(folder);
    fs::create_directories(folder);
    EXPECT_EQ(GameBuildId(folder), "");
    { std::ofstream(folder / "pak01_dir.vpk", std::ios::binary) << "abcd"; }
    const std::string from_archive = GameBuildId(folder);
    EXPECT_EQ(from_archive.rfind("archive 4 ", 0), 0u) << from_archive;
    { std::ofstream(folder / "pak01_dir.vpk", std::ios::binary) << "abcdefgh"; }
    EXPECT_NE(GameBuildId(folder), from_archive);
    { std::ofstream(folder / "steam.inf", std::ios::binary) << "ClientVersion=6011\r\n"; }
    EXPECT_EQ(GameBuildId(folder), "client 6011");
    std::error_code error;
    fs::remove_all(folder, error);
}

TEST(GameFolderTests, ParsesEscapedLibraryPaths) {
    const std::string vdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		""
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
		"apps"
		{
			"1422450"		"40000000000"
		}
	}
})";
    EXPECT_EQ(ParseSteamLibraryPaths(vdf), (std::vector<std::string>{"C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary"}));
}

TEST(GameFolderTests, IgnoresFilesWithoutLibraryPaths) {
    EXPECT_TRUE(ParseSteamLibraryPaths("").empty());
    EXPECT_TRUE(ParseSteamLibraryPaths(R"("libraryfolders" { "0" { "label" "" } })").empty());
}

}  // namespace
