#include "library_model.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using skeleton_repair::BlobId;
using skeleton_repair::FilterModels;
using skeleton_repair::FormatHistory;
using skeleton_repair::FormatModelRow;
using skeleton_repair::FormatUnproven;
using skeleton_repair::History;
using skeleton_repair::HistoryLink;
using skeleton_repair::HistoryVersion;
using skeleton_repair::ModelRow;
using skeleton_repair::SlotObservation;
using skeleton_repair::kUnprovenSlotHeading;

BlobId Blob(uint8_t fill) {
    BlobId id;
    id.Bytes.fill(fill);
    return id;
}

TEST(LibraryModel, TwoPackDuplicateUsesSnapshotCount) {
    ModelRow row;
    row.Blob = Blob(0xab);
    row.SnapshotCount = 2;
    row.PathCount = 1;
    row.DisplayPath = "models/heroes/hero.vmdl_c";
    EXPECT_EQ(FormatModelRow(row), "abababababab  in 2 packs  models/heroes/hero.vmdl_c");
}

TEST(LibraryModel, MutatedHistoryLineCarriesRelationAndDetail) {
    History history;
    HistoryVersion root;
    root.Blob = Blob(0x11);
    root.Paths = {"models/a.vmdl_c"};
    HistoryVersion mutated;
    mutated.Blob = Blob(0x22);
    mutated.Paths = {"models/a.vmdl_c"};
    HistoryLink link;
    link.From = root.Blob;
    link.To = mutated.Blob;
    link.Relation = "mutated";
    link.Detail = "mutate-a";
    history.Versions = {root, mutated};
    history.Links = {link};

    const std::vector<std::string> lines = FormatHistory(history);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_TRUE(lines[0].ends_with("imported"));
    EXPECT_EQ(lines[0].find("mutated"), std::string::npos);
    EXPECT_NE(lines[1].find("mutated"), std::string::npos);
    EXPECT_NE(lines[1].find("mutate-a"), std::string::npos);
    EXPECT_EQ(lines[0].substr(0, 12), root.Blob.Hex().substr(0, 12));
    EXPECT_EQ(lines[1].substr(0, 12), mutated.Blob.Hex().substr(0, 12));
}

TEST(LibraryModel, UnprovenTextStartsWithTheHeading) {
    SlotObservation slot;
    slot.Blob = Blob(0x33);
    slot.Path = "models/a.vmdl_c";
    slot.PackLabel = "other.vpk";
    const std::string text = FormatUnproven({slot});
    EXPECT_TRUE(text.starts_with(kUnprovenSlotHeading));
    EXPECT_NE(text.find("models/a.vmdl_c"), std::string::npos);
    EXPECT_NE(text.find("other.vpk"), std::string::npos);
    EXPECT_EQ(FormatUnproven({}), std::string(kUnprovenSlotHeading));
}

TEST(LibraryModel, FilterIsACaseInsensitivePathSubstring) {
    ModelRow hero;
    hero.Blob = Blob(0x01);
    hero.DisplayPath = "models/Heroes/hero.vmdl_c";
    ModelRow side;
    side.Blob = Blob(0x02);
    side.DisplayPath = "models/side/side.vmdl_c";
    const std::vector<ModelRow> rows{hero, side};

    const std::vector<ModelRow> all = FilterModels(rows, "");
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].DisplayPath, hero.DisplayPath);
    EXPECT_EQ(all[1].DisplayPath, side.DisplayPath);

    const std::vector<ModelRow> matched = FilterModels(rows, "HERO");
    ASSERT_EQ(matched.size(), 1u);
    EXPECT_EQ(matched[0].DisplayPath, hero.DisplayPath);
    EXPECT_TRUE(FilterModels(rows, "missing").empty());
}

}  // namespace
