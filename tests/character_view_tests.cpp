#include "character_view.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using skeleton_repair::CharacterOfModel;
using skeleton_repair::CharacterSlots;
using skeleton_repair::CharactersIn;
using skeleton_repair::GridForViewportCount;
using skeleton_repair::LibraryModel;
using skeleton_repair::ModelFileTitle;
using skeleton_repair::SlotCaption;

LibraryModel Model(const char* archive, const char* path) {
    return {archive, path};
}

TEST(CharacterView, HeroFolderNamesTheCharacter) {
    EXPECT_EQ(CharacterOfModel("models/heroes/mina/mina.vmdl_c"), "mina");
    EXPECT_EQ(CharacterOfModel("models/heroes_staging/Mina/skins/mina_ult.vmdl_c"), "Mina");
    EXPECT_EQ(CharacterOfModel("models\\Heroes\\haze\\haze.vmdl_c"), "haze");
    EXPECT_FALSE(CharacterOfModel("models/heroes/mina").has_value());
    EXPECT_FALSE(CharacterOfModel("models/npc/mina/mina.vmdl_c").has_value());
    EXPECT_FALSE(CharacterOfModel("models/hero/mina/mina.vmdl_c").has_value());
}

TEST(CharacterView, CaptionUsesTheArchiveAndTheModelTitle) {
    EXPECT_EQ(ModelFileTitle("models/heroes/mina/mina_ult.vmdl_c"), "mina ult");
    const LibraryModel model = Model("C:/mods/Mina Pack.vpk", "models/heroes/mina/mina_ult.vmdl_c");
    EXPECT_EQ(SlotCaption(model), "Mina Pack.vpk - mina ult");
}

TEST(CharacterView, GridIsOneRowUntilFourModels) {
    EXPECT_EQ(GridForViewportCount(0).Columns, 1);
    EXPECT_EQ(GridForViewportCount(1).Rows, 1);
    EXPECT_EQ(GridForViewportCount(2).Columns, 2);
    EXPECT_EQ(GridForViewportCount(2).Rows, 1);
    EXPECT_EQ(GridForViewportCount(3).Columns, 3);
    EXPECT_EQ(GridForViewportCount(4).Columns, 2);
    EXPECT_EQ(GridForViewportCount(4).Rows, 2);
    EXPECT_EQ(GridForViewportCount(9).Columns, 2);
    EXPECT_EQ(GridForViewportCount(9).Rows, 2);
}

TEST(CharacterView, SlotsPreferOneModelFromEachMod) {
    const std::vector<LibraryModel> models = {
        Model("Mods/A.vpk", "models/heroes/mina/mina.vmdl_c"),
        Model("Mods/A.vpk", "models/heroes/mina/mina_ult.vmdl_c"),
        Model("Mods/B.vpk", "models/heroes/mina/mina.vmdl_c"),
        Model("Mods/C.vpk", "models/heroes/mina/mina_ghost.vmdl_c"),
        Model("Mods/A.vpk", "models/heroes/haze/haze.vmdl_c"),
    };
    EXPECT_EQ(CharacterSlots(models, 0, 4), (std::vector<size_t>{0, 2, 3, 1}));
    EXPECT_EQ(CharacterSlots(models, 0, 2), (std::vector<size_t>{0, 2}));
}

TEST(CharacterView, OneModFillsTheGridWithItsOtherModels) {
    const std::vector<LibraryModel> models = {
        Model("Mods/A.vpk", "models/heroes/mina/mina_z.vmdl_c"),
        Model("Mods/A.vpk", "models/heroes/mina/mina_a.vmdl_c"),
        Model("Mods/A.vpk", "models/heroes/mina/mina.vmdl_c"),
        Model("Mods/A.vpk", "models/heroes/mina/mina_b.vmdl_c"),
    };
    EXPECT_EQ(CharacterSlots(models, 2, 4), (std::vector<size_t>{2, 1, 3, 0}));
}

TEST(CharacterView, SameArchiveSpellingIsOneMod) {
    const std::vector<LibraryModel> models = {
        Model("Mods/A.vpk", "models/heroes/mina/mina.vmdl_c"),
        Model("mods/a.vpk", "models/heroes/mina/mina_ult.vmdl_c"),
        Model("Mods/B.vpk", "models/heroes/mina/mina.vmdl_c"),
    };
    EXPECT_EQ(CharacterSlots(models, 0, 4), (std::vector<size_t>{0, 2, 1}));
}

TEST(CharacterView, CharacterListCountsAndSorts) {
    const std::vector<LibraryModel> models = {
        Model("A.vpk", "models/heroes/mina/mina.vmdl_c"),
        Model("B.vpk", "models/Heroes/Mina/mina_ult.vmdl_c"),
        Model("C.vpk", "models/heroes/haze/haze.vmdl_c"),
        Model("D.vpk", "materials/mina.vtex_c"),
    };
    const auto characters = CharactersIn(models);
    ASSERT_EQ(characters.size(), 2u);
    EXPECT_EQ(characters[0].Name, "haze");
    EXPECT_EQ(characters[0].Count, 1u);
    EXPECT_EQ(characters[1].Name, "mina");
    EXPECT_EQ(characters[1].Count, 2u);
}

TEST(CharacterView, MissingCharacterStillShowsTheChosenModel) {
    const std::vector<LibraryModel> models = {Model("A.vpk", "models/props/box.vmdl_c")};
    EXPECT_EQ(CharacterSlots(models, 0, 4), (std::vector<size_t>{0}));
    EXPECT_TRUE(CharactersIn(models).empty());
    EXPECT_TRUE(CharacterSlots(models, 3, 4).empty());
    EXPECT_TRUE(CharacterSlots(models, 0, 0).empty());
}

}  // namespace
