#include "store_reader.hpp"

#include <gtest/gtest.h>

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
