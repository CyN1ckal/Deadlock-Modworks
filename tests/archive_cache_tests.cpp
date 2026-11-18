#include "archive_cache.hpp"
#include "latest_task.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace {

using namespace skeleton_repair;
namespace fs = std::filesystem;

class ArchiveCacheTest : public testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("archive-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { fs::create_directories(dir); }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(dir, error);
    }
    fs::path Archive(const char* name, const std::vector<VpkChange>& entries) {
        const auto path = dir / name;
        std::vector<uint8_t> seed(29);
        const uint32_t header[] = {0x55aa1234, 2, 1};
        std::memcpy(seed.data(), header, sizeof(header));
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        }
        VpkArchive::Open(path).Write(path, entries);
        return path;
    }

    // Polls until the task's result is ready.
    template <typename T>
    static std::optional<T> Wait(LatestTask<T>& task) {
        for (int i = 0; i < 500; ++i) {
            if (auto result = task.Take()) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }
};

TEST_F(ArchiveCacheTest, IndexedArchiveFindsPathsIgnoringCase) {
    const IndexedArchive archive(VpkArchive::Open(Archive("mod.vpk", {{"models/Hero/Body.vmdl_c", std::vector<uint8_t>{1, 2}}})));
    ASSERT_NE(archive.Find("MODELS/hero/body.VMDL_C"), nullptr);
    EXPECT_EQ(archive.Read("models/hero/body.vmdl_c"), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(archive.Find("models/hero/missing.vmdl_c"), nullptr);
    EXPECT_EQ(archive.Read("models/hero/missing.vmdl_c"), std::nullopt);
}

TEST_F(ArchiveCacheTest, CacheOpensEachArchiveOnceUntilRemoved) {
    const auto path = Archive("mod.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    ArchiveCache cache;
    const auto first = cache.Get(path);
    // Another spelling of the same file is the same archive.
    EXPECT_EQ(cache.Get(dir / "." / "MOD.vpk"), first);
    cache.Remove({path});
    const auto reopened = cache.Get(path);
    EXPECT_NE(reopened, first);
    // Holders keep their copy after the cache forgets it.
    EXPECT_NE(first->Find("a.txt"), nullptr);
}

TEST_F(ArchiveCacheTest, FailedOpenIsRetried) {
    const auto path = dir / "later.vpk";
    ArchiveCache cache;
    EXPECT_ANY_THROW(cache.Get(path));
    Archive("later.vpk", {{"a.txt", std::vector<uint8_t>{1}}});
    EXPECT_NE(cache.Get(path), nullptr);
}

TEST_F(ArchiveCacheTest, LatestTaskReturnsOnlyTheNewestResult) {
    LatestTask<int> task;
    std::atomic<bool> release = false;
    task.Start([&](CancellationToken token) {
        while (!release && !token.IsCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 1;
    });
    task.Start([](CancellationToken) { return 2; });
    release = true;
    EXPECT_EQ(Wait(task), 2);
    EXPECT_FALSE(task.Busy());
}

TEST_F(ArchiveCacheTest, LatestTaskRethrowsAndCancels) {
    LatestTask<int> task;
    task.Start([](CancellationToken) -> int { throw std::runtime_error("broken"); });
    for (int i = 0; i < 500 && task.Busy(); ++i) {
        try {
            if (task.Take()) break;
        } catch (const std::runtime_error& error) {
            EXPECT_STREQ(error.what(), "broken");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(task.Busy());
    task.Start([](CancellationToken) { return 3; });
    task.Cancel(true);
    EXPECT_FALSE(task.Busy());
    EXPECT_EQ(task.Take(), std::nullopt);
}

}  // namespace
