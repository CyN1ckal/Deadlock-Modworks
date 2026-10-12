#include "kv3.hpp"
#include "model.hpp"
#include "resource.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::ModelMigration;
using skeleton_repair::ModelOutcome;
using skeleton_repair::ReadKv3;

constexpr const char* kPath = "models/heroes/test/test.vmdl_c";

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> Wrap(const std::vector<uint8_t>& data, const char* extra_block) {
    const int count = extra_block == nullptr ? 1 : 2;
    const int start = 16 + count * 12;
    std::vector<uint8_t> file(static_cast<size_t>(start) + data.size() + (extra_block == nullptr ? 0u : 4u));
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, count);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, static_cast<uint32_t>(start - 20));
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    if (extra_block != nullptr) {
        std::memcpy(file.data() + 28, extra_block, 4);
        WriteU32(file, 32, static_cast<uint32_t>(start + data.size() - 32));
        WriteI32(file, 36, 4);
        file[start + data.size()] = 1;
        file[start + data.size() + 1] = 2;
        file[start + data.size() + 2] = 3;
        file[start + data.size() + 3] = 4;
    }
    return file;
}

KvValue Strings(const std::vector<std::string>& values) {
    std::vector<KvValue> items;
    for (const std::string& value : values) {
        items.push_back(KvValue::String(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Ints(const std::vector<int>& values) {
    std::vector<KvValue> items;
    for (int value : values) {
        items.push_back(KvValue::Int32(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Vec(float x) {
    return KvValue::Array({KvValue::Float(x), KvValue::Float(0), KvValue::Float(0)});
}

KvValue Rotation() {
    return KvValue::Array({KvValue::Float(0), KvValue::Float(0), KvValue::Float(0), KvValue::Float(1)});
}

struct ModelSpec {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    std::optional<std::vector<int>> Remap;
    float Pose = 0;
    std::vector<int> Flags;
    std::string SkeletonRef;
    std::string UnknownKey;
    const char* ExtraBlock = nullptr;
};

std::vector<uint8_t> Model(const ModelSpec& spec) {
    std::vector<KvValue> positions;
    std::vector<KvValue> rotations;
    std::vector<KvValue> spheres;
    std::vector<KvValue> scales;
    std::vector<int> flags = spec.Flags;
    flags.resize(spec.Names.size(), 0);
    for (size_t i = 0; i < spec.Names.size(); ++i) {
        positions.push_back(Vec(spec.Pose));
        rotations.push_back(Rotation());
        spheres.push_back(KvValue::Float(1));
        scales.push_back(KvValue::Float(1));
    }
    KvValue skeleton = KvValue::Object({});
    skeleton.Set("m_boneName", Strings(spec.Names));
    skeleton.Set("m_nParent", Ints(spec.Parents));
    skeleton.Set("m_boneSphere", KvValue::Array(std::move(spheres)));
    skeleton.Set("m_nFlag", Ints(flags));
    skeleton.Set("m_bonePosParent", KvValue::Array(std::move(positions)));
    skeleton.Set("m_boneRotParent", KvValue::Array(std::move(rotations)));
    skeleton.Set("m_boneScaleParent", KvValue::Array(std::move(scales)));
    if (!spec.UnknownKey.empty()) {
        skeleton.Set(spec.UnknownKey, KvValue::Int32(1));
    }
    KvValue root = KvValue::Object({});
    root.Set("m_name", KvValue::String("models/heroes/test/test.vmdl"));
    root.Set("m_modelSkeleton", std::move(skeleton));
    if (!spec.SkeletonRef.empty()) {
        root.Set("m_vecNmSkeletonRefs", KvValue::Array({KvValue::String(spec.SkeletonRef)}));
    }
    if (spec.Remap.has_value()) {
        root.Set("m_remappingTable", Ints(*spec.Remap));
    }
    KvDocument document;
    document.Root = std::move(root);
    return Wrap(skeleton_repair::WriteKv3(document), spec.ExtraBlock);
}

const KvValue& Field(const KvValue& root, const char* key) {
    const KvValue* value = root.Find(key);
    EXPECT_NE(value, nullptr);
    return *value;
}

}  // namespace

TEST(Model, ReorderRemapsSkinningAndKeepsAuthoredPoses) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto mod = Model({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = std::vector<int>{0, 1}, .Pose = 5});
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    ASSERT_EQ(edit.Outcome, ModelOutcome::Updated) << edit.Detail;
    EXPECT_EQ(edit.Detail,
        "Bones 2 to 2 (2 from the game, 0 kept from the mod). Mesh bone indices were remapped. "
        "Game-only bones: 0; mod-only bones: 0. "
        "Test animations and attachments in-game before keeping the repair.");
    ASSERT_TRUE(edit.Bytes.has_value());

    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& skeleton = Field(checked.Root, "m_modelSkeleton");
    EXPECT_EQ(skeleton.Find("m_boneName")->At(0).String(), "root");
    EXPECT_EQ(skeleton.Find("m_boneName")->At(1).String(), "arm");
    EXPECT_EQ(skeleton.Find("m_nParent")->At(0).Int32(), -1);
    EXPECT_EQ(skeleton.Find("m_nParent")->At(1).Int32(), 0);
    EXPECT_FLOAT_EQ(skeleton.Find("m_bonePosParent")->At(0).At(0).Float(), 5.0f);
    EXPECT_FLOAT_EQ(skeleton.Find("m_bonePosParent")->At(1).At(0).Float(), 5.0f);
    EXPECT_EQ(Field(checked.Root, "m_remappingTable").At(0).Int32(), 1);
    EXPECT_EQ(Field(checked.Root, "m_remappingTable").At(1).Int32(), 0);

    const auto again = ModelMigration::Migrate(*edit.Bytes, kPath, game, kPath);
    EXPECT_EQ(again.Outcome, ModelOutcome::Current);
    EXPECT_FALSE(again.Bytes.has_value());
}

TEST(Model, AddedGameBoneKeepsTheModPoseAndRemap) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto mod = Model({.Names = {"root"}, .Parents = {-1}, .Remap = std::vector<int>{0}, .Pose = 5});
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    ASSERT_EQ(edit.Outcome, ModelOutcome::Updated) << edit.Detail;
    EXPECT_EQ(edit.Detail,
        "Bones 1 to 2 (2 from the game, 0 kept from the mod). Mesh bone indices were remapped. "
        "Game-only bones: 1; mod-only bones: 0. Game-only examples: arm. "
        "Test animations and attachments in-game before keeping the repair.");
    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& skeleton = Field(checked.Root, "m_modelSkeleton");
    EXPECT_EQ(skeleton.Find("m_boneName")->At(1).String(), "arm");
    EXPECT_EQ(skeleton.Find("m_nParent")->At(1).Int32(), 0);
    EXPECT_FLOAT_EQ(skeleton.Find("m_bonePosParent")->At(0).At(0).Float(), 5.0f);
    EXPECT_FLOAT_EQ(skeleton.Find("m_bonePosParent")->At(1).At(0).Float(), 0.0f);
    EXPECT_EQ(Field(checked.Root, "m_remappingTable").At(0).Int32(), 0);
}

TEST(Model, CustomHelperStaysWhenTheGamePrefixMatches) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto mod = Model({.Names = {"root", "arm", "custom_helper"}, .Parents = {-1, 0, 1}, .Remap = std::vector<int>{0, 1, 2}, .Pose = 3});
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    EXPECT_EQ(edit.Outcome, ModelOutcome::Current);
    EXPECT_FALSE(edit.Bytes.has_value());
    EXPECT_NE(edit.Detail.find("custom_helper"), std::string::npos);
    ASSERT_TRUE(edit.Warnings.has_value());
    ASSERT_EQ(edit.Warnings->size(), 1u);
    EXPECT_NE(edit.Warnings->at(0).find("Custom rest poses"), std::string::npos);
}

TEST(Model, Ag2RenderRigIsNotRewritten) {
    const auto game = Model({
        .Names = {"root", "arm"},
        .Parents = {-1, 0},
        .Remap = std::vector<int>{0, 1},
        .SkeletonRef = "models/heroes/test/test.vnmskel",
    });
    const auto mod = Model({
        .Names = {"arm", "root", "custom_helper"},
        .Parents = {1, -1, 1},
        .Remap = std::vector<int>{0, 1, 2},
        .Pose = 5,
        .SkeletonRef = "models/heroes/test/test.vnmskel",
    });
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    EXPECT_EQ(edit.Outcome, ModelOutcome::Current);
    EXPECT_FALSE(edit.Bytes.has_value());
    EXPECT_TRUE(edit.Detail.starts_with("External animation skeleton bindings are used. No render-skeleton rewrite is needed."));
    ASSERT_TRUE(edit.Warnings.has_value());
    EXPECT_FALSE(edit.Warnings->empty());
}

TEST(Model, ReviewAndFailureGates) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto missing = Model({.Names = {"arm", "root"}, .Parents = {1, -1}});
    const auto missing_edit = ModelMigration::Migrate(missing, kPath, game, kPath);
    EXPECT_EQ(missing_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(missing_edit.Detail, "Skeleton differs, but no mesh bone remap table is available. Direct mesh indices require author review.");

    const auto phys = Model({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = std::vector<int>{0, 1}, .ExtraBlock = "PHYS"});
    const auto phys_edit = ModelMigration::Migrate(phys, kPath, game, kPath);
    EXPECT_EQ(phys_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(phys_edit.Detail, "Skeleton differs and the model has embedded PHYS data. Its bone references need author review.");

    const auto unknown = Model({.Names = {"arm", "root"}, .Parents = {1, -1}, .Remap = std::vector<int>{0, 1}, .UnknownKey = "m_custom"});
    const auto unknown_edit = ModelMigration::Migrate(unknown, kPath, game, kPath);
    EXPECT_EQ(unknown_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(unknown_edit.Detail, "Skeleton contains unsupported metadata: m_custom. Its bone references need author review.");

    const auto same = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .UnknownKey = "m_custom"});
    EXPECT_EQ(ModelMigration::Migrate(same, kPath, game, kPath).Outcome, ModelOutcome::Current);

    const auto moved = ModelMigration::Migrate(game, "models/heroes/old/test.vmdl_c", game, kPath);
    EXPECT_EQ(moved.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(moved.Detail, "Possible model move to models/heroes/test/test.vmdl_c. Path migration requires checking dependent resources; this MVP will not guess.");

    const auto cycle = Model({.Names = {"root", "arm"}, .Parents = {1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto cycle_edit = ModelMigration::Migrate(cycle, kPath, game, kPath);
    EXPECT_EQ(cycle_edit.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(cycle_edit.Detail, "Skeleton has a parent cycle at root.");

    const auto bad_remap = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{2}});
    const auto bad_edit = ModelMigration::Migrate(bad_remap, kPath, game, kPath);
    EXPECT_EQ(bad_edit.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(bad_edit.Detail, "Mesh remapping entry 0 points outside the skeleton.");

    const auto empty = Model({.Names = {}, .Parents = {}});
    EXPECT_EQ(ModelMigration::Migrate(empty, kPath, game, kPath).Outcome, ModelOutcome::Skipped);

    const auto no_game = ModelMigration::Migrate(game, kPath, empty, kPath);
    EXPECT_EQ(no_game.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(no_game.Detail, "The game model has no bones.");

    const auto broken = ModelMigration::Migrate(std::vector<uint8_t>{1, 2, 3}, kPath, game, kPath);
    EXPECT_EQ(broken.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(broken.Detail, "Resource is too small.");
}

TEST(Model, FlagDifferencesStayWithTheMod) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .Flags = {0, 4}});
    const auto mod = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}, .Flags = {0, 1}});
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    EXPECT_EQ(edit.Outcome, ModelOutcome::Current);
    ASSERT_TRUE(edit.Warnings.has_value());
    ASSERT_EQ(edit.Warnings->size(), 1u);
    EXPECT_EQ(edit.Warnings->at(0), "Authored bone usage/LOD flags differ (1); mod flags are preserved.");
}

TEST(Model, HelperParentIsRemappedWithTheGameOrder) {
    const auto game = Model({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Remap = std::vector<int>{0, 1}});
    const auto mod = Model({
        .Names = {"arm", "root", "custom_helper"},
        .Parents = {1, -1, 0},
        .Remap = std::vector<int>{0, 1, 2},
        .Pose = 4,
    });
    const auto edit = ModelMigration::Migrate(mod, kPath, game, kPath);
    ASSERT_EQ(edit.Outcome, ModelOutcome::Updated) << edit.Detail;
    EXPECT_NE(edit.Detail.find("1 kept from the mod"), std::string::npos);
    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& skeleton = Field(checked.Root, "m_modelSkeleton");
    EXPECT_EQ(skeleton.Find("m_boneName")->At(2).String(), "custom_helper");
    EXPECT_EQ(skeleton.Find("m_nParent")->At(2).Int32(), 1);
    EXPECT_FLOAT_EQ(skeleton.Find("m_bonePosParent")->At(2).At(0).Float(), 4.0f);
    EXPECT_EQ(Field(checked.Root, "m_remappingTable").At(0).Int32(), 1);
    EXPECT_EQ(Field(checked.Root, "m_remappingTable").At(2).Int32(), 2);
}
