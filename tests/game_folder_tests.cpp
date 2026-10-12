#include "game_folder.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using skeleton_repair::ParseSteamLibraryPaths;

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
