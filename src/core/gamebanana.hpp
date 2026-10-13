#pragma once

#include "cancellation.hpp"
#include "http_client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// GameBanana's id for Deadlock. Mods of other games are refused.
inline constexpr int64_t kGameBananaDeadlock = 20948;

struct GbModSummary {
    int64_t Id = 0;
    std::string Name;
    std::string ProfileUrl;
    std::string Submitter;
    std::string Category;
    std::string Version;
    // A 220 px wide preview image, or empty.
    std::string ThumbnailUrl;
    int64_t DateAdded = 0;
    int64_t DateModified = 0;
    int64_t Likes = 0;
    int64_t Views = 0;
    // The submitter marked the mod with a content rating, such as mature content.
    bool Rated = false;
    bool Obsolete = false;
};

struct GbFile {
    int64_t Id = 0;
    std::string Name;
    uint64_t Size = 0;
    int64_t DateAdded = 0;
    int64_t Downloads = 0;
    std::string DownloadUrl;
    // Lowercase hex MD5 of the file, or empty when GameBanana has none.
    std::string Md5;
    std::string Description;
    // GameBanana's antivirus result, such as "clean".
    std::string ScanResult;
};

struct GbMod : GbModSummary {
    int64_t GameId = 0;
    // The mod page's description as HTML.
    std::string Text;
    // 530 px wide screenshots.
    std::vector<std::string> Images;
    std::vector<GbFile> Files;
};

struct GbPage {
    std::vector<GbModSummary> Mods;
    int64_t Total = 0;
    int PerPage = 0;
    bool Complete = true;
};

struct GbCategory {
    int64_t Id = 0;
    std::string Name;
    int64_t Count = 0;
};

enum class GbSort { Newest, Updated, MostDownloaded, MostLiked, MostViewed };

// The mod id from a GameBanana mod link (gamebanana.com/mods/123, /mods/download/123, a mod manager link
// ending in ",Mod,123") or a plain number. Links to other GameBanana pages give nothing.
std::optional<int64_t> ParseGameBananaModId(std::string_view text);

std::string GbBrowseUrl(GbSort sort, int page, int per_page, int64_t category = 0);
std::string GbSearchUrl(std::string_view query, int page);
std::string GbModUrl(int64_t id);
std::string GbCategoriesUrl();

// Parse API responses. InvalidData when the text is not the expected JSON.
GbPage ParseGbPage(std::string_view json);
GbMod ParseGbMod(std::string_view json);
std::vector<GbCategory> ParseGbCategories(std::string_view json);

// Network calls to gamebanana.com. They throw like HttpGet.
namespace gamebanana {
GbPage Browse(GbSort sort, int page, int64_t category = 0, CancellationToken cancellation = {});
GbPage Search(std::string_view query, int page, CancellationToken cancellation = {});
// InvalidData when the mod is not a Deadlock mod.
GbMod Mod(int64_t id, CancellationToken cancellation = {});
std::vector<GbCategory> Categories(CancellationToken cancellation = {});
// Downloads one of the mod's files and checks its MD5 when GameBanana lists one.
void Download(const GbFile& file, const std::filesystem::path& destination, CancellationToken cancellation = {},
    const DownloadProgress& progress = {});
// A preview image's bytes, from GameBanana's image hosts only.
std::string Image(std::string_view url, CancellationToken cancellation = {});
}  // namespace gamebanana

}  // namespace skeleton_repair
