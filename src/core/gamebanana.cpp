#include "gamebanana.hpp"

#include "error.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::string_view kApi = "https://gamebanana.com/apiv11/";

HttpOptions ApiOptions() { return {{"gamebanana.com"}, 16ull << 20}; }

std::string Text(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}
int64_t Number(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? found->get<int64_t>() : 0;
}
bool Flag(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_boolean() && found->get<bool>();
}
const json& Object(const json& object, const char* key) {
    static const json empty = json::object();
    const auto found = object.find(key);
    return found != object.end() && found->is_object() ? *found : empty;
}

json ParseJson(std::string_view text) {
    auto parsed = json::parse(text.begin(), text.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) throw InvalidData("GameBanana sent a response Modworks could not read.");
    if (parsed.contains("_sErrorCode")) throw InvalidData("GameBanana refused the request (" + Text(parsed, "_sErrorCode") + ").");
    return parsed;
}

// Screenshot URLs of a preview media object, at the given size prefix ("_sFile220", "_sFile530").
std::vector<std::string> Images(const json& record, const char* size) {
    std::vector<std::string> result;
    const auto& media = Object(record, "_aPreviewMedia");
    const auto images = media.find("_aImages");
    if (images == media.end() || !images->is_array()) return result;
    for (const auto& image : *images) {
        if (!image.is_object()) continue;
        const auto base = Text(image, "_sBaseUrl");
        auto file = Text(image, size);
        if (file.empty()) file = Text(image, "_sFile");
        if (!base.empty() && !file.empty()) result.push_back(base + "/" + file);
    }
    return result;
}

void ReadSummary(const json& record, GbModSummary& mod) {
    mod.Id = Number(record, "_idRow");
    mod.Name = Text(record, "_sName");
    mod.ProfileUrl = Text(record, "_sProfileUrl");
    mod.Submitter = Text(Object(record, "_aSubmitter"), "_sName");
    mod.Category = Text(Object(record, "_aRootCategory"), "_sName");
    if (mod.Category.empty()) mod.Category = Text(Object(record, "_aCategory"), "_sName");
    mod.Version = Text(record, "_sVersion");
    mod.DateAdded = Number(record, "_tsDateAdded");
    mod.DateModified = Number(record, "_tsDateModified");
    mod.Likes = Number(record, "_nLikeCount");
    mod.Views = Number(record, "_nViewCount");
    mod.Rated = Flag(record, "_bHasContentRatings") || (record.contains("_aContentRatings") && !record["_aContentRatings"].empty());
    mod.Obsolete = Flag(record, "_bIsObsolete");
    const auto thumbnails = Images(record, "_sFile220");
    if (!thumbnails.empty()) mod.ThumbnailUrl = thumbnails.front();
}

std::string Encode(std::string_view text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else { result += '%'; result += digits[c >> 4]; result += digits[c & 15]; }
    }
    return result;
}

std::string SortName(GbSort sort) {
    switch (sort) {
    case GbSort::Updated: return "Generic_LatestUpdated";
    case GbSort::MostDownloaded: return "Generic_MostDownloaded";
    case GbSort::MostLiked: return "Generic_MostLiked";
    case GbSort::MostViewed: return "Generic_MostViewed";
    case GbSort::Newest: break;
    }
    return "Generic_Newest";
}

std::string FileMd5(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) throw IoError("Could not check the download.");
    std::array<uint8_t, 16> digest{};
    try {
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw IoError("Could not check the download.");
        std::ifstream in(path, std::ios::binary);
        std::vector<char> buffer(1 << 16);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto read = static_cast<ULONG>(in.gcount());
            if (read && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) < 0) throw IoError("Could not check the download.");
        }
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw IoError("Could not check the download.");
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

int64_t ParseNumber(std::string_view digits) {
    int64_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return error == std::errc() && end == digits.data() + digits.size() && value > 0 ? value : 0;
}
}  // namespace

std::optional<int64_t> ParseGameBananaModId(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        const auto id = ParseNumber(text);
        return id ? std::optional(id) : std::nullopt;
    }
    // A mod manager link: "<scheme>:https://gamebanana.com/mmdl/<file>,Mod,<id>".
    if (const auto marker = text.rfind(",Mod,"); marker != std::string_view::npos) {
        const auto id = ParseNumber(text.substr(marker + 5));
        return id ? std::optional(id) : std::nullopt;
    }
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string_view prefix : {"https://", "http://"}) if (lower.starts_with(prefix)) { lower.erase(0, prefix.size()); break; }
    if (lower.starts_with("www.")) lower.erase(0, 4);
    if (!lower.starts_with("gamebanana.com/")) return std::nullopt;
    std::string_view path = std::string_view(lower).substr(std::string_view("gamebanana.com/").size());
    if (const auto end = path.find_first_of("?#"); end != std::string_view::npos) path = path.substr(0, end);
    for (const std::string_view prefix : {"mods/download/", "mods/embeddables/", "mods/"}) {
        if (!path.starts_with(prefix)) continue;
        auto rest = path.substr(prefix.size());
        if (const auto slash = rest.find('/'); slash != std::string_view::npos) rest = rest.substr(0, slash);
        const auto id = ParseNumber(rest);
        return id ? std::optional(id) : std::nullopt;
    }
    return std::nullopt;
}

std::string GbBrowseUrl(GbSort sort, int page, int per_page, int64_t category) {
    std::string url = std::string(kApi) + "Mod/Index?_nPage=" + std::to_string(std::max(page, 1)) + "&_nPerpage=" + std::to_string(std::clamp(per_page, 1, 50))
        + "&_aFilters%5BGeneric_Game%5D=" + std::to_string(kGameBananaDeadlock) + "&_sSort=" + SortName(sort);
    if (category > 0) url += "&_aFilters%5BGeneric_Category%5D=" + std::to_string(category);
    return url;
}

std::string GbSearchUrl(std::string_view query, int page) {
    return std::string(kApi) + "Util/Search/Results?_sModelName=Mod&_sOrder=best_match&_idGameRow=" + std::to_string(kGameBananaDeadlock)
        + "&_sSearchString=" + Encode(query) + "&_nPage=" + std::to_string(std::max(page, 1));
}

std::string GbModUrl(int64_t id) { return std::string(kApi) + "Mod/" + std::to_string(id) + "/ProfilePage"; }

std::string GbCategoriesUrl() {
    return std::string(kApi) + "Mod/Categories?_idGameRow=" + std::to_string(kGameBananaDeadlock) + "&_sSort=a_to_z&_bShowEmpty=false";
}

GbPage ParseGbPage(std::string_view text) {
    const auto parsed = ParseJson(text);
    GbPage page;
    const auto& metadata = Object(parsed, "_aMetadata");
    page.Total = Number(metadata, "_nRecordCount");
    page.PerPage = static_cast<int>(Number(metadata, "_nPerpage"));
    page.Complete = Flag(metadata, "_bIsComplete");
    const auto records = parsed.find("_aRecords");
    if (records == parsed.end() || !records->is_array()) throw InvalidData("GameBanana sent a response Modworks could not read.");
    for (const auto& record : *records) {
        if (!record.is_object() || Text(record, "_sModelName") != "Mod") continue;
        GbModSummary mod;
        ReadSummary(record, mod);
        if (mod.Id > 0) page.Mods.push_back(std::move(mod));
    }
    return page;
}

GbMod ParseGbMod(std::string_view text) {
    const auto parsed = ParseJson(text);
    GbMod mod;
    ReadSummary(parsed, mod);
    if (mod.Id <= 0) throw InvalidData("GameBanana did not return a mod.");
    mod.GameId = Number(Object(parsed, "_aGame"), "_idRow");
    mod.Text = Text(parsed, "_sText");
    mod.Images = Images(parsed, "_sFile530");
    const auto files = parsed.find("_aFiles");
    if (files != parsed.end() && files->is_array()) {
        for (const auto& record : *files) {
            if (!record.is_object()) continue;
            GbFile file;
            file.Id = Number(record, "_idRow");
            file.Name = Text(record, "_sFile");
            file.Size = static_cast<uint64_t>(std::max<int64_t>(Number(record, "_nFilesize"), 0));
            file.DateAdded = Number(record, "_tsDateAdded");
            file.Downloads = Number(record, "_nDownloadCount");
            file.DownloadUrl = Text(record, "_sDownloadUrl");
            file.Md5 = Text(record, "_sMd5Checksum");
            std::transform(file.Md5.begin(), file.Md5.end(), file.Md5.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            file.Description = Text(record, "_sDescription");
            file.ScanResult = Text(record, "_sAvResult");
            if (file.Id > 0 && !file.DownloadUrl.empty()) mod.Files.push_back(std::move(file));
        }
    }
    // Newest file first, which is the one most people want.
    std::stable_sort(mod.Files.begin(), mod.Files.end(), [](const GbFile& a, const GbFile& b) { return a.DateAdded > b.DateAdded; });
    return mod;
}

std::vector<GbCategory> ParseGbCategories(std::string_view text) {
    auto parsed = json::parse(text.begin(), text.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) throw InvalidData("GameBanana sent a response Modworks could not read.");
    std::vector<GbCategory> result;
    for (const auto& record : parsed) {
        if (!record.is_object()) continue;
        GbCategory category{Number(record, "_idRow"), Text(record, "_sName"), Number(record, "_nItemCount")};
        if (category.Id > 0 && !category.Name.empty()) result.push_back(std::move(category));
    }
    return result;
}

namespace gamebanana {
GbPage Browse(GbSort sort, int page, int64_t category, CancellationToken cancellation) {
    return ParseGbPage(HttpGet(GbBrowseUrl(sort, page, 30, category), ApiOptions(), cancellation));
}

GbPage Search(std::string_view query, int page, CancellationToken cancellation) {
    return ParseGbPage(HttpGet(GbSearchUrl(query, page), ApiOptions(), cancellation));
}

GbMod Mod(int64_t id, CancellationToken cancellation) {
    auto mod = ParseGbMod(HttpGet(GbModUrl(id), ApiOptions(), cancellation));
    if (mod.GameId != kGameBananaDeadlock) throw InvalidData("That GameBanana page is not a Deadlock mod.");
    return mod;
}

std::vector<GbCategory> Categories(CancellationToken cancellation) {
    return ParseGbCategories(HttpGet(GbCategoriesUrl(), ApiOptions(), cancellation));
}

void Download(const GbFile& file, const fs::path& destination, CancellationToken cancellation, const DownloadProgress& progress) {
    HttpOptions options{{"gamebanana.com"}, 4ull << 30};
    HttpDownload(file.DownloadUrl, destination, options, cancellation, progress);
    if (!file.Md5.empty() && FileMd5(destination) != file.Md5) {
        std::error_code error;
        fs::remove(destination, error);
        throw InvalidData("The download of " + file.Name + " does not match GameBanana's checksum. Try again.");
    }
}

std::string Image(std::string_view url, CancellationToken cancellation) {
    return HttpGet(url, {{"gamebanana.com"}, 8ull << 20}, cancellation);
}
}  // namespace gamebanana

}  // namespace skeleton_repair
