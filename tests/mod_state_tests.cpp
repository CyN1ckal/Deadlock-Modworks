#include "error.hpp"
#include "mod_state.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;
using namespace skeleton_repair;
using Ids = std::vector<std::string>;

struct TempFolder {
    fs::path Path = fs::temp_directory_path() / ("mod-state-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed())
        + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    TempFolder() { fs::remove_all(Path); fs::create_directories(Path); }
    ~TempFolder() { std::error_code error; fs::remove_all(Path, error); }
};

TEST(ModState, StartsWithOneEmptyProfile) {
    const ModState state;
    ASSERT_EQ(state.Profiles().size(), 1u);
    EXPECT_EQ(state.ActiveProfile(), "Default");
    EXPECT_TRUE(state.Order().empty());
}

TEST(ModState, AddsNewModsAtTheEndOfTheLoadOrder) {
    ModState state;
    state.Sync({"b", "a"});
    state.Sync({"c", "a"});
    EXPECT_EQ(state.Order(), (Ids{"b", "a", "c"}));
    EXPECT_EQ(state.Ordered({"c", "b"}), (Ids{"b", "c"}));
}

TEST(ModState, EnabledModsFollowTheLoadOrder) {
    ModState state;
    state.Sync({"a", "b", "c"});
    state.SetEnabled("c", true);
    state.SetEnabled("a", true);
    EXPECT_EQ(state.EnabledInOrder({"a", "b", "c"}), (Ids{"a", "c"}));
    EXPECT_EQ(state.EnabledInOrder({"c"}), (Ids{"c"}));
    state.SetEnabled("a", false);
    EXPECT_FALSE(state.Enabled("a"));
    EXPECT_EQ(state.EnabledInOrder({"a", "b", "c"}), (Ids{"c"}));
}

TEST(ModState, ReordersMods) {
    ModState state;
    state.Sync({"a", "b", "c", "d"});
    state.MoveBefore("d", "a");
    EXPECT_EQ(state.Order(), (Ids{"d", "a", "b", "c"}));
    state.MoveBefore("d", "");
    EXPECT_EQ(state.Order(), (Ids{"a", "b", "c", "d"}));
    // Stepping skips mods that are not present.
    state.Step("c", true, {"a", "c", "d"});
    EXPECT_EQ(state.Order(), (Ids{"c", "a", "b", "d"}));
    state.Step("c", false, {"a", "c", "d"});
    EXPECT_EQ(state.Ordered({"a", "c", "d"}), (Ids{"a", "c", "d"}));
    state.Step("d", false, {"a", "c", "d"});
    EXPECT_EQ(state.Ordered({"a", "c", "d"}), (Ids{"a", "c", "d"}));
}

TEST(ModState, ProfilesKeepTheirOwnEnabledMods) {
    ModState state;
    state.Sync({"a", "b"});
    state.SetEnabled("a", true);
    state.CreateProfile("Ranked", false);
    EXPECT_EQ(state.ActiveProfile(), "Ranked");
    EXPECT_FALSE(state.Enabled("a"));
    state.SetEnabled("b", true);
    state.CreateProfile(" Copy ", true);
    EXPECT_EQ(state.ActiveProfile(), "Copy");
    EXPECT_TRUE(state.Enabled("b"));
    state.SwitchProfile("Default");
    EXPECT_TRUE(state.Enabled("a"));
    EXPECT_FALSE(state.Enabled("b"));
    EXPECT_THROW(state.CreateProfile("ranked", false), InvalidOperation);
    EXPECT_THROW(state.CreateProfile("  ", false), InvalidOperation);
    EXPECT_THROW(state.SwitchProfile("Missing"), InvalidOperation);
}

TEST(ModState, RenamesAndDeletesProfiles) {
    ModState state;
    state.CreateProfile("Two", false);
    state.RenameProfile("Two", "Casual");
    EXPECT_EQ(state.ActiveProfile(), "Casual");
    EXPECT_THROW(state.RenameProfile("Casual", "default"), InvalidOperation);
    state.DeleteProfile("Casual");
    EXPECT_EQ(state.ActiveProfile(), "Default");
    EXPECT_THROW(state.DeleteProfile("Default"), InvalidOperation);
}

TEST(ModState, ForgetsRemovedMods) {
    ModState state;
    state.Sync({"a", "b"});
    state.SetEnabled("a", true);
    state.SetOrigin("a", {7, 8, "a.zip", "https://gamebanana.com/mods/7", 99});
    state.Forget("a");
    EXPECT_EQ(state.Order(), (Ids{"b"}));
    EXPECT_FALSE(state.Enabled("a"));
    EXPECT_FALSE(state.Origin("a"));
}

TEST(ModState, SavesAndLoads) {
    TempFolder temp;
    const auto file = temp.Path / "mods.json";
    EXPECT_EQ(ModState::Load(file), ModState());
    ModState state;
    state.Sync({"a", "b", "c"});
    state.SetEnabled("b", true);
    state.CreateProfile("Other", false);
    state.SetEnabled("c", true);
    state.SetOrigin("c", {691764, 1746166, "haze.zip", "https://gamebanana.com/mods/691764", 1783303978});
    state.Save(file);
    const auto loaded = ModState::Load(file);
    EXPECT_EQ(loaded, state);
    EXPECT_EQ(loaded.FromGameBanana(691764), (Ids{"c"}));
    EXPECT_FALSE(fs::exists(temp.Path / "mods.json.tmp"));
}

TEST(ModState, RefusesDamagedFilesAndLeavesThem) {
    TempFolder temp;
    const auto file = temp.Path / "mods.json";
    std::ofstream(file) << "{ not json";
    EXPECT_THROW(ModState::Load(file), InvalidData);
    std::ofstream(file, std::ios::trunc) << R"({"version": 1, "order": [3]})";
    EXPECT_THROW(ModState::Load(file), InvalidData);
    std::ofstream(file, std::ios::trunc) << R"({"version": 99})";
    EXPECT_THROW(ModState::Load(file), InvalidData);
    EXPECT_TRUE(fs::exists(file));
}

TEST(ModState, LoadRepairsMissingProfilesAndOrder) {
    TempFolder temp;
    const auto file = temp.Path / "mods.json";
    std::ofstream(file) << R"({"version": 1, "order": ["a"], "active": "Gone",
        "profiles": [{"name": "P", "enabled": ["b"]}, {"name": "P", "enabled": []}]})";
    const auto state = ModState::Load(file);
    EXPECT_EQ(state.ActiveProfile(), "P");
    ASSERT_EQ(state.Profiles().size(), 1u);
    EXPECT_EQ(state.Order(), (Ids{"a", "b"}));
    EXPECT_TRUE(state.Enabled("b"));
}

}  // namespace
