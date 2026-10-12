#include "thumbnail_cache.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>

namespace {
using namespace skeleton_repair;
namespace fs = std::filesystem;
class ThumbnailCacheTest : public testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("thumbnail-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool com = false;
    void SetUp() override {
        fs::create_directories(dir);
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }
    void TearDown() override {
        if (com) CoUninitialize();
        std::error_code error;
        fs::remove_all(dir, error);
    }
    void Write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
};

TEST_F(ThumbnailCacheTest, SavedImageLoadsBackOpaque) {
    ThumbnailPixels pixels{3, 2, {}};
    for (uint8_t i = 0; i < 6; ++i) pixels.Rgba.insert(pixels.Rgba.end(), {static_cast<uint8_t>(i * 40), 20, static_cast<uint8_t>(250 - i), 255});
    const auto path = dir / "nested" / "thumb.png";
    SaveThumbnailImage(path, pixels);
    const auto loaded = LoadThumbnailImage(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->Width, 3u);
    EXPECT_EQ(loaded->Height, 2u);
    EXPECT_EQ(loaded->Rgba, pixels.Rgba);
}

TEST_F(ThumbnailCacheTest, MissingOrDamagedImageIsNotLoaded) {
    EXPECT_FALSE(LoadThumbnailImage(dir / "missing.png"));
    const auto damaged = dir / "damaged.png";
    Write(damaged, "not a png");
    EXPECT_FALSE(LoadThumbnailImage(damaged));
    EXPECT_FALSE(fs::exists(damaged));
}

TEST_F(ThumbnailCacheTest, PathChangesWithTheArchive) {
    const auto archive = dir / "mod.vpk";
    Write(archive, "one");
    const LibraryModel model{archive, "models/heroes/hero.vmdl_c"};
    const auto first = ThumbnailCachePath(dir, model, {});
    EXPECT_EQ(first, ThumbnailCachePath(dir, {archive, "Models/Heroes/Hero.vmdl_c"}, {}));
    EXPECT_NE(first, ThumbnailCachePath(dir, {archive, "models/heroes/other.vmdl_c"}, {}));
    Write(archive, "changed");
    EXPECT_NE(first, ThumbnailCachePath(dir, model, {}));
}
}
