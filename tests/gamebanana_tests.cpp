#include "error.hpp"
#include "gamebanana.hpp"
#include "http_client.hpp"
#include "archive_extract.hpp"
#include "vpk.hpp"

#include <filesystem>

#include <gtest/gtest.h>

namespace {

using namespace skeleton_repair;

TEST(GameBananaLinks, ReadsModIdsFromLinksAndNumbers) {
    EXPECT_EQ(ParseGameBananaModId("https://gamebanana.com/mods/691764"), 691764);
    EXPECT_EQ(ParseGameBananaModId("  gamebanana.com/mods/691764/  "), 691764);
    EXPECT_EQ(ParseGameBananaModId("https://www.GameBanana.com/mods/download/691764#FileInfo_1746166"), 691764);
    EXPECT_EQ(ParseGameBananaModId("http://gamebanana.com/mods/691764?tab=files"), 691764);
    EXPECT_EQ(ParseGameBananaModId("deadlock-mod-manager:https://gamebanana.com/mmdl/1746166,Mod,691764"), 691764);
    EXPECT_EQ(ParseGameBananaModId("691764"), 691764);
}

TEST(GameBananaLinks, RefusesOtherLinks) {
    EXPECT_FALSE(ParseGameBananaModId(""));
    EXPECT_FALSE(ParseGameBananaModId("0"));
    EXPECT_FALSE(ParseGameBananaModId("https://gamebanana.com/sounds/94090"));
    EXPECT_FALSE(ParseGameBananaModId("https://gamebanana.com/mods/cats/31713x"));
    EXPECT_FALSE(ParseGameBananaModId("https://evil.example/gamebanana.com/mods/1"));
    EXPECT_FALSE(ParseGameBananaModId("https://gamebanana.com.evil.example/mods/1"));
    EXPECT_FALSE(ParseGameBananaModId("haze skin"));
}

TEST(GameBananaLinks, BuildsApiUrls) {
    EXPECT_EQ(GbModUrl(42), "https://gamebanana.com/apiv11/Mod/42/ProfilePage");
    const auto browse = GbBrowseUrl(GbSort::MostDownloaded, 2, 30, 31713);
    EXPECT_NE(browse.find("_nPage=2"), std::string::npos);
    EXPECT_NE(browse.find("_sSort=Generic_MostDownloaded"), std::string::npos);
    EXPECT_NE(browse.find("Generic_Game%5D=20948"), std::string::npos);
    EXPECT_NE(browse.find("Generic_Category%5D=31713"), std::string::npos);
    const auto search = GbSearchUrl("haze & lady", 1);
    EXPECT_NE(search.find("_sSearchString=haze%20%26%20lady"), std::string::npos);
    EXPECT_NE(search.find("_idGameRow=20948"), std::string::npos);
}

constexpr const char* kPage = R"json({
    "_aMetadata": {"_nRecordCount": 161, "_nPerpage": 15, "_bIsComplete": false},
    "_aRecords": [
        {"_idRow": 691764, "_sModelName": "Mod", "_sName": "haze black recolor",
         "_sProfileUrl": "https:\/\/gamebanana.com\/mods\/691764", "_tsDateAdded": 1783253542, "_tsDateModified": 1783303978,
         "_aPreviewMedia": {"_aImages": [{"_sBaseUrl": "https:\/\/images.gamebanana.com\/img\/ss\/mods", "_sFile": "a.jpg", "_sFile220": "220-90_a.jpg"}]},
         "_aSubmitter": {"_sName": "v__v"}, "_aRootCategory": {"_sName": "Skins"}, "_nLikeCount": 6, "_nViewCount": 3445,
         "_bHasContentRatings": false, "_sVersion": "1.1"},
        {"_idRow": 94090, "_sModelName": "Sound", "_sName": "a sound"},
        {"_idRow": 5, "_sModelName": "Mod", "_sName": "rated", "_bHasContentRatings": true, "_bIsObsolete": true}
    ]
})json";

TEST(GameBananaParse, ReadsPagesOfMods) {
    const auto page = ParseGbPage(kPage);
    EXPECT_EQ(page.Total, 161);
    EXPECT_EQ(page.PerPage, 15);
    EXPECT_FALSE(page.Complete);
    ASSERT_EQ(page.Mods.size(), 2u);
    const auto& mod = page.Mods[0];
    EXPECT_EQ(mod.Id, 691764);
    EXPECT_EQ(mod.Name, "haze black recolor");
    EXPECT_EQ(mod.Submitter, "v__v");
    EXPECT_EQ(mod.Category, "Skins");
    EXPECT_EQ(mod.Version, "1.1");
    EXPECT_EQ(mod.ThumbnailUrl, "https://images.gamebanana.com/img/ss/mods/220-90_a.jpg");
    EXPECT_EQ(mod.Likes, 6);
    EXPECT_FALSE(mod.Rated);
    EXPECT_TRUE(page.Mods[1].Rated);
    EXPECT_TRUE(page.Mods[1].Obsolete);
}

TEST(GameBananaParse, ReadsModPagesWithNewestFileFirst) {
    const auto mod = ParseGbMod(R"json({
        "_idRow": 691764, "_sName": "haze black recolor", "_aGame": {"_idRow": 20948},
        "_aCategory": {"_sName": "Haze"}, "_sText": "<b>hi</b>",
        "_aPreviewMedia": {"_aImages": [{"_sBaseUrl": "https://images.gamebanana.com/img/ss/mods", "_sFile": "a.jpg", "_sFile530": "530-90_a.jpg"}]},
        "_aFiles": [
            {"_idRow": 1, "_sFile": "old.zip", "_nFilesize": 10, "_tsDateAdded": 100, "_sDownloadUrl": "https://gamebanana.com/dl/1", "_sMd5Checksum": "ABCDEF", "_sAvResult": "clean"},
            {"_idRow": 2, "_sFile": "new.7z", "_nFilesize": 20, "_tsDateAdded": 200, "_sDownloadUrl": "https://gamebanana.com/dl/2"},
            {"_idRow": 3, "_sFile": "no-link.zip"}
        ]
    })json");
    EXPECT_EQ(mod.GameId, kGameBananaDeadlock);
    EXPECT_EQ(mod.Category, "Haze");
    ASSERT_EQ(mod.Images.size(), 1u);
    EXPECT_EQ(mod.Images[0], "https://images.gamebanana.com/img/ss/mods/530-90_a.jpg");
    ASSERT_EQ(mod.Files.size(), 2u);
    EXPECT_EQ(mod.Files[0].Name, "new.7z");
    EXPECT_EQ(mod.Files[1].Md5, "abcdef");
    EXPECT_EQ(mod.Files[1].ScanResult, "clean");
}

TEST(GameBananaParse, RefusesUnexpectedResponses) {
    EXPECT_THROW(ParseGbPage("<html>"), InvalidData);
    EXPECT_THROW(ParseGbPage(R"({"_sErrorCode": "INPUT_ERRORS"})"), InvalidData);
    EXPECT_THROW(ParseGbMod(R"({"_sName": "no id"})"), InvalidData);
    EXPECT_THROW(ParseGbCategories("{}"), InvalidData);
    const auto categories = ParseGbCategories(R"([{"_idRow": 47611, "_sName": "Animations", "_nItemCount": 19}, {"_sName": "no id"}])");
    ASSERT_EQ(categories.size(), 1u);
    EXPECT_EQ(categories[0].Name, "Animations");
}

TEST(GameBananaParse, TurnsDescriptionsIntoPlainText) {
    EXPECT_EQ(GbPlainText("<b>Black</b> haze &amp; friends<br>Line two<br /><ul><li>One</li><li>Two</li></ul>"),
        "Black haze & friends\nLine two\n- One\n- Two");
    EXPECT_EQ(GbPlainText("<p>a</p><p></p><p></p><p>b</p>"), "a\n\nb");
    EXPECT_EQ(GbPlainText("1 &lt; 2 &unknown; <unclosed"), "1 < 2 &unknown;");
}

TEST(HttpUrls, AcceptsOnlyPlainHttps) {
    const auto url = ParseHttpsUrl("https://GameBanana.com/apiv11/Mod/1?x=1#frag");
    ASSERT_TRUE(url);
    EXPECT_EQ(url->Host, "gamebanana.com");
    EXPECT_EQ(url->Target, "/apiv11/Mod/1?x=1");
    EXPECT_EQ(ParseHttpsUrl("https://files.gamebanana.com")->Target, "/");
    EXPECT_EQ(ParseHttpsUrl("https://files.gamebanana.com?q")->Target, "/?q");
    EXPECT_FALSE(ParseHttpsUrl("http://gamebanana.com/"));
    EXPECT_FALSE(ParseHttpsUrl("https://user@gamebanana.com/"));
    EXPECT_FALSE(ParseHttpsUrl("https://gamebanana.com:8443/"));
    EXPECT_FALSE(ParseHttpsUrl("https:///path"));
    EXPECT_FALSE(ParseHttpsUrl("ftp://gamebanana.com/"));
}

TEST(HttpUrls, MatchesDomainsAndSubdomainsOnly) {
    EXPECT_TRUE(HostWithin("gamebanana.com", "gamebanana.com"));
    EXPECT_TRUE(HostWithin("files.gamebanana.com", "gamebanana.com"));
    EXPECT_TRUE(HostWithin("Images.GameBanana.com", "gamebanana.com"));
    EXPECT_FALSE(HostWithin("evilgamebanana.com", "gamebanana.com"));
    EXPECT_FALSE(HostWithin("gamebanana.com.evil.example", "gamebanana.com"));
}

// Talks to gamebanana.com, so it only runs with --gtest_also_run_disabled_tests.
TEST(GameBananaLive, DISABLED_DownloadsAndExtractsARealMod) {
    namespace fs = std::filesystem;
    const auto page = gamebanana::Search("haze", 1);
    ASSERT_FALSE(page.Mods.empty());
    const auto mod = gamebanana::Mod(691764);
    EXPECT_EQ(mod.Name, "haze black recolor");
    ASSERT_FALSE(mod.Files.empty());
    const auto folder = fs::temp_directory_path() / "gamebanana-live";
    fs::remove_all(folder);
    fs::create_directories(folder);
    const auto archive = folder / mod.Files[0].Name;
    uint64_t received = 0;
    gamebanana::Download(mod.Files[0], archive, {}, [&](uint64_t done, std::optional<uint64_t>) { received = done; });
    EXPECT_EQ(received, mod.Files[0].Size);
    const auto extracted = ExtractModArchive(archive, folder / "out");
    ASSERT_FALSE(extracted.Archives.empty());
    EXPECT_FALSE(VpkArchive::Open(extracted.Archives[0]).Entries().empty());
    EXPECT_FALSE(gamebanana::Image(mod.ThumbnailUrl).empty());
    EXPECT_FALSE(gamebanana::Categories().empty());
    fs::remove_all(folder);
}

}  // namespace
