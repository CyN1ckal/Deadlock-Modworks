#include "store_reader.hpp"

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

TEST(StoreReader, ClipsStayUnderTheModelFolder) {
    const std::vector<std::string> candidates{
        "models/heroes/clips/idle.vnmclip_c",
        "models/heroes/clip/run.vnmclip_c",
        "models/heroes/hero.vmdl_c",
        "models/other/idle.vnmclip_c",
        "models/heroes/clips/idle.vnmclip_c",
    };
    const std::vector<std::string> clips = skeleton_repair::ClipsUnderModel("models/heroes/hero.vmdl_c", candidates);
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0], "models/heroes/clips/idle.vnmclip_c");
    EXPECT_EQ(clips[1], "models/heroes/clip/run.vnmclip_c");
}

TEST(StoreReader, PreferredSnapshotWinsOtherwiseLatestObservation) {
    skeleton_repair::MembershipRow early;
    early.Snapshot.Value = "aaa";
    early.Path = "models/a.vmdl_c";
    early.ObservedUtc = "2020-01-01T00:00:00Z";
    skeleton_repair::MembershipRow later;
    later.Snapshot.Value = "bbb";
    later.Path = "models/b.vmdl_c";
    later.ObservedUtc = "2024-01-01T00:00:00Z";
    const std::vector<skeleton_repair::MembershipRow> rows{early, later};

    const skeleton_repair::MembershipRow* latest = skeleton_repair::VersionMembership(rows, nullptr);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->Snapshot.Value, "bbb");

    skeleton_repair::SnapshotId preferred;
    preferred.Value = "aaa";
    const skeleton_repair::MembershipRow* chosen = skeleton_repair::VersionMembership(rows, &preferred);
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->Snapshot.Value, "aaa");

    skeleton_repair::SnapshotId missing;
    missing.Value = "ccc";
    const skeleton_repair::MembershipRow* fallback = skeleton_repair::VersionMembership(rows, &missing);
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->Snapshot.Value, "bbb");
}

namespace {

// A snapshot reader with no store behind it. The second path throws, like a blob that is listed but unreadable.
class FakeSnapshots final : public skeleton_repair::SnapshotReader {
public:
    std::optional<std::vector<uint8_t>> ReadSnapshotPath(
        const skeleton_repair::SnapshotId& snapshot, std::string_view path) const override {
        if (path == "materials/broken.vtex_c") {
            throw std::runtime_error("blob missing");
        }
        const auto found = Files.find(snapshot.Value + "/" + std::string(path));
        if (found == Files.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    std::map<std::string, std::vector<uint8_t>> Files;
};

}  // namespace

TEST(StoreReader, SnapshotBytesWinThenGameThenNothing) {
    FakeSnapshots snapshots;
    snapshots.Files["aaa/models/hero.vmdl_c"] = {1, 2};
    skeleton_repair::SnapshotId snapshot;
    snapshot.Value = "aaa";
    const auto game = [](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (path == "models/only_in_game.vmdl_c" || path == "materials/broken.vtex_c") {
            return std::vector<uint8_t>{9};
        }
        return std::nullopt;
    };

    EXPECT_EQ(skeleton_repair::ReadSnapshotThenGame(snapshots, &snapshot, "models/hero.vmdl_c", game), (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(skeleton_repair::ReadSnapshotThenGame(snapshots, &snapshot, "models/only_in_game.vmdl_c", game), (std::vector<uint8_t>{9}));
    EXPECT_EQ(skeleton_repair::ReadSnapshotThenGame(snapshots, &snapshot, "materials/broken.vtex_c", game), (std::vector<uint8_t>{9}));
    EXPECT_EQ(skeleton_repair::ReadSnapshotThenGame(snapshots, nullptr, "models/hero.vmdl_c", game), std::nullopt);
    EXPECT_EQ(skeleton_repair::ReadSnapshotThenGame(snapshots, &snapshot, "models/missing.vmdl_c", {}), std::nullopt);
}
