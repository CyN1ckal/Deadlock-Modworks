#include "animation.hpp"
#include "kv3.hpp"
#include "resource.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using skeleton_repair::AnimationSkeletonMigration;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::ModelOutcome;
using skeleton_repair::ReadKv3;
using skeleton_repair::WriteKv3;

constexpr const char* kPath = "models/heroes/test/test.vnmskel_c";
constexpr const char* kId = "models/heroes/test/test.vnmskel";

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<uint8_t> Wrap(const std::vector<uint8_t>& data, bool trailing_block) {
    const int count = trailing_block ? 2 : 1;
    const int start = 16 + count * 12;
    std::vector<uint8_t> file(static_cast<size_t>(start) + data.size() + (trailing_block ? 4u : 0u));
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
    if (trailing_block) {
        file[28] = 'P';
        file[29] = 'H';
        file[30] = 'Y';
        file[31] = 'S';
        WriteU32(file, 32, static_cast<uint32_t>(start + data.size() - 32));
        WriteI32(file, 36, 4);
        file[start + data.size()] = 1;
        file[start + data.size() + 1] = 2;
        file[start + data.size() + 2] = 3;
        file[start + data.size() + 3] = 4;
    }
    return file;
}

KvValue Pose(float x, float y = 0, float z = 0, float scale = 1, float qx = 0, float qy = 0, float qz = 0, float qw = 1) {
    return KvValue::Array({
        KvValue::Float(x), KvValue::Float(y), KvValue::Float(z), KvValue::Float(scale),
        KvValue::Float(qx), KvValue::Float(qy), KvValue::Float(qz), KvValue::Float(qw),
    });
}

KvValue Names(const std::vector<std::string>& names) {
    std::vector<KvValue> values;
    for (const std::string& name : names) {
        values.push_back(KvValue::String(name));
    }
    return KvValue::Array(std::move(values));
}

KvValue Parents(const std::vector<int>& parents) {
    std::vector<KvValue> values;
    for (int parent : parents) {
        values.push_back(KvValue::Int32(parent));
    }
    return KvValue::Array(std::move(values));
}

struct AnimSpec {
    std::vector<std::string> Names;
    std::vector<int> Parents;
    int Lod = 1;
    float PoseX = 0;
    std::vector<KvValue> Poses;
    std::string Id = kId;
    std::string MaskLabel = "custom-mask";
    std::string OtherMaskSkeleton;
    bool Masks = true;
    bool SecondaryItem = false;
    std::string UnknownKey;
    bool Trailing = false;
};

std::vector<uint8_t> Animation(const AnimSpec& spec) {
    KvValue root = KvValue::Object({});
    root.Set("m_ID", KvValue::String(spec.Id));
    root.Set("m_boneIDs", Names(spec.Names));
    root.Set("m_parentIndices", Parents(spec.Parents));
    std::vector<KvValue> poses;
    for (size_t i = 0; i < spec.Names.size(); ++i) {
        if (!spec.Poses.empty()) {
            poses.push_back(spec.Poses[i]);
        } else {
            poses.push_back(Pose(spec.PoseX));
        }
    }
    root.Set("m_parentSpaceReferencePose", KvValue::Array(poses));
    root.Set("m_modelSpaceReferencePose", KvValue::Array(poses));
    root.Set("m_numBonesToSampleAtLowLOD", KvValue::Int32(spec.Lod));
    if (spec.Masks) {
        KvValue weights = KvValue::Object({});
        weights.Set("m_skeletonName", KvValue::String(spec.Id));
        weights.Set("m_boneIDs", Names(spec.Names));
        std::vector<KvValue> weight_values(spec.Names.size(), KvValue::Float(0.5f));
        weights.Set("m_weights", KvValue::Array(std::move(weight_values)));
        KvValue mask = KvValue::Object({});
        mask.Set("m_ID", KvValue::String(spec.MaskLabel));
        mask.Set("m_authoredValue", KvValue::Float(0.75f));
        mask.Set("m_primaryWeightList", std::move(weights));
        if (!spec.OtherMaskSkeleton.empty()) {
            KvValue other = KvValue::Object({});
            other.Set("m_skeletonName", KvValue::String(spec.OtherMaskSkeleton));
            other.Set("m_boneIDs", Names(spec.Names));
            std::vector<KvValue> other_weights(spec.Names.size(), KvValue::Float(0.25f));
            other.Set("m_weights", KvValue::Array(std::move(other_weights)));
            mask.Set("m_secondaryWeightList", std::move(other));
        }
        root.Set("m_maskDefinitions", KvValue::Array({std::move(mask)}));
    }
    if (spec.SecondaryItem) {
        root.Set("m_secondarySkeletons", KvValue::Array({KvValue::String("models/other.vnmskel")}));
    } else {
        root.Set("m_secondarySkeletons", KvValue::Array({}));
    }
    root.Set("m_bIsPropSkeleton", KvValue::Bool(false));
    if (!spec.UnknownKey.empty()) {
        root.Set(spec.UnknownKey, KvValue::Int32(1));
    }
    KvDocument document;
    document.Root = std::move(root);
    return Wrap(WriteKv3(document), spec.Trailing);
}

const KvValue& Field(const KvValue& root, const char* key) {
    const KvValue* value = root.Find(key);
    EXPECT_NE(value, nullptr);
    return *value;
}

uint32_t Bits(float value) {
    return std::bit_cast<uint32_t>(value);
}

}  // namespace

TEST(Animation, ReordersBonesAndRebuildsWorldPoses) {
    const auto mod = Animation({.Names = {"arm", "root"}, .Parents = {1, -1}, .Lod = 2, .PoseX = 7, .Trailing = true});
    const auto game = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto edit = AnimationSkeletonMigration::Migrate(mod, kPath, game, kPath);

    EXPECT_EQ(edit.Outcome, ModelOutcome::Updated);
    ASSERT_TRUE(edit.Bytes.has_value());
    ASSERT_TRUE(edit.Warnings.has_value());
    EXPECT_EQ(edit.Warnings->size(), 1u);
    EXPECT_EQ(edit.Warnings->at(0), "Test this animation-skeleton repair in-game; structural verification cannot confirm animation playback.");
    EXPECT_EQ(edit.Detail,
        "Animation override binding layout updated: 2 to 3 bones; low-LOD sampling 2 to 2. "
        "Authored parent-space poses and mask weights are preserved, world poses are rebuilt, and model geometry is untouched.");
    EXPECT_EQ(edit.Bytes->at(edit.Bytes->size() - 4), 1);
    EXPECT_EQ(edit.Bytes->back(), 4);

    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& bones = Field(checked.Root, "m_boneIDs");
    EXPECT_EQ(bones.Count(), 3u);
    EXPECT_EQ(bones.At(0).String(), "root");
    EXPECT_EQ(bones.At(1).String(), "arm");
    EXPECT_EQ(bones.At(2).String(), "attachWorld");
    EXPECT_EQ(Field(checked.Root, "m_parentIndices").At(0).Int32(), -1);
    EXPECT_EQ(Field(checked.Root, "m_parentIndices").At(1).Int32(), 0);
    EXPECT_EQ(Field(checked.Root, "m_numBonesToSampleAtLowLOD").Int32(), 2);
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_parentSpaceReferencePose").At(0).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_parentSpaceReferencePose").At(1).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_parentSpaceReferencePose").At(2).At(0).Float(), 0.0f);
    const KvValue& world = Field(checked.Root, "m_modelSpaceReferencePose");
    EXPECT_FLOAT_EQ(world.At(0).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(world.At(1).At(0).Float(), 14.0f);
    EXPECT_FLOAT_EQ(world.At(2).At(0).Float(), 7.0f);
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_maskDefinitions").At(0).Find("m_authoredValue")->Float(), 0.75f);
    ASSERT_TRUE(edit.BoneAliases.has_value());
    EXPECT_TRUE(edit.BoneAliases->empty());
}

TEST(Animation, SpunWorldPosePreservesExpectedFloatBits) {
    const KvValue spun = Pose(1, 2, 3, 1.5f, 0, 0.70710677f, 0, 0.70710677f);
    const auto mod = Animation({.Names = {"root"}, .Parents = {-1}, .Lod = 1, .Poses = {spun}});
    const auto game = Animation({.Names = {"root", "child"}, .Parents = {-1, 0}, .Lod = 1});
    const auto edit = AnimationSkeletonMigration::Migrate(mod, kPath, game, kPath);
    ASSERT_EQ(edit.Outcome, ModelOutcome::Updated) << edit.Detail;
    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& world = Field(checked.Root, "m_modelSpaceReferencePose");
    const uint32_t expected[] = {0x3F800000u, 0x40000000u, 0x40400000u, 0x3FBFFFFFu, 0x00000000u, 0x3F3504F2u, 0x00000000u, 0x3F3504F4u};
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(Bits(world.At(0).At(i).Float()), expected[i]) << i;
        EXPECT_EQ(Bits(world.At(1).At(i).Float()), expected[i]) << i;
    }
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_parentSpaceReferencePose").At(0).At(3).Float(), 1.5f);
}

TEST(Animation, MatchingBindingIsCurrent) {
    const auto file = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 2, .PoseX = 4, .UnknownKey = "m_custom"});
    const auto edit = AnimationSkeletonMigration::Migrate(file, kPath, file, "Models/Heroes/Test/Test.vnmskel_c");
    EXPECT_EQ(edit.Outcome, ModelOutcome::Current);
    EXPECT_FALSE(edit.Bytes.has_value());
    EXPECT_EQ(edit.Detail, "Animation bone order, parent bindings and low-LOD sampling match the installed game. Authored poses and masks are preserved.");
}

TEST(Animation, PathOrContainerProblemsDoNotRewrite) {
    const auto file = Animation({.Names = {"root"}, .Parents = {-1}, .Lod = 1});
    const auto moved = AnimationSkeletonMigration::Migrate(file, "old/test.vnmskel_c", file, kPath);
    EXPECT_EQ(moved.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(moved.Detail, "Animation skeleton path migration is not supported.");

    std::vector<uint8_t> foreign = Wrap(std::vector<uint8_t>{1, 2, 3, 4}, false);
    const auto plain = AnimationSkeletonMigration::Migrate(foreign, kPath, file, kPath);
    EXPECT_EQ(plain.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(plain.Detail, "Animation skeleton does not use a supported KV3 DATA block.");

    const auto broken = AnimationSkeletonMigration::Migrate(std::span<const uint8_t>{}, kPath, file, kPath);
    EXPECT_EQ(broken.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(broken.Detail, "Resource is too small.");
}

TEST(Animation, CustomIdentityOrParentsNeedReview) {
    const auto game = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 2});
    const auto custom = Animation({.Names = {"root", "custom_helper"}, .Parents = {-1, 0}, .Lod = 2});
    const auto custom_edit = AnimationSkeletonMigration::Migrate(custom, kPath, game, kPath);
    EXPECT_EQ(custom_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(custom_edit.Detail, "Animation override has custom or removed bones: custom_helper. An explicit mapping is needed.");

    auto other_id = Animation({.Names = {"root", "arm", "attachWorld"}, .Parents = {-1, 0, 0}, .Lod = 1, .Id = "models/heroes/test/custom.vnmskel"});
    const auto id_edit = AnimationSkeletonMigration::Migrate(other_id, kPath, game, kPath);
    EXPECT_EQ(id_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(id_edit.Detail, "Animation skeleton identities differ; this may be a custom skeleton.");

    const auto changed = Animation({.Names = {"root", "arm"}, .Parents = {-1, -1}, .Lod = 2});
    const auto parent_edit = AnimationSkeletonMigration::Migrate(changed, kPath, game, kPath);
    EXPECT_EQ(parent_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(parent_edit.Detail, "Animation override parents changed: arm. Retargeting needs author review.");
}

TEST(Animation, BadParentIndexFails) {
    const auto game = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    const auto bad = Animation({.Names = {"root", "arm"}, .Parents = {-1, 8}, .Lod = 1});
    const auto edit = AnimationSkeletonMigration::Migrate(bad, kPath, game, kPath);
    EXPECT_EQ(edit.Outcome, ModelOutcome::Failed);
    EXPECT_EQ(edit.Detail, "Animation parent index is outside the skeleton.");
    EXPECT_FALSE(edit.Bytes.has_value());
}

TEST(Animation, CaseRenameUpdatesOnlyTheMatchingMask) {
    const auto game = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"},
        .Parents = {-1, 0, 1, 0},
        .Lod = 2,
    });
    const auto mod = Animation({
        .Names = {"root", "weapon_hand_l", "magazine_l"},
        .Parents = {-1, 0, 1},
        .Lod = 3,
        .PoseX = 7,
        .MaskLabel = "weapon_hand_l",
        .OtherMaskSkeleton = "models/other.vnmskel",
    });
    const auto edit = AnimationSkeletonMigration::Migrate(mod, kPath, game, kPath);
    ASSERT_EQ(edit.Outcome, ModelOutcome::Updated) << edit.Detail;
    EXPECT_EQ(edit.Detail,
        "Animation override binding layout updated: 3 to 4 bones; low-LOD sampling 3 to 2. "
        "Authored parent-space poses and mask weights are preserved, world poses are rebuilt, and model geometry is untouched. "
        "Case-only bone renames (2): weapon_hand_l -> weapon_hand_L, magazine_l -> magazine_L; updated 2 mask references.");
    ASSERT_TRUE(edit.BoneAliases.has_value());
    ASSERT_EQ(edit.BoneAliases->size(), 2u);
    EXPECT_EQ(edit.BoneAliases->at(0).first, "weapon_hand_l");
    EXPECT_EQ(edit.BoneAliases->at(0).second, "weapon_hand_L");

    const KvDocument checked = ReadKv3(skeleton_repair::FindDataBlock(*edit.Bytes));
    const KvValue& bones = Field(checked.Root, "m_boneIDs");
    EXPECT_EQ(bones.At(1).String(), "weapon_hand_L");
    EXPECT_EQ(bones.At(2).String(), "magazine_L");
    EXPECT_FLOAT_EQ(Field(checked.Root, "m_parentSpaceReferencePose").At(1).At(0).Float(), 7.0f);
    const KvValue& mask = Field(checked.Root, "m_maskDefinitions").At(0);
    EXPECT_EQ(mask.Find("m_ID")->String(), "weapon_hand_l");
    EXPECT_EQ(mask.Find("m_primaryWeightList")->Find("m_boneIDs")->At(1).String(), "weapon_hand_L");
    EXPECT_FLOAT_EQ(mask.Find("m_primaryWeightList")->Find("m_weights")->At(1).Float(), 0.5f);
    EXPECT_EQ(mask.Find("m_secondaryWeightList")->Find("m_boneIDs")->At(1).String(), "weapon_hand_l");
    EXPECT_FLOAT_EQ(mask.Find("m_secondaryWeightList")->Find("m_weights")->At(1).Float(), 0.25f);
}

TEST(Animation, CasingWithChangedParentsNeedsReview) {
    const auto game = Animation({
        .Names = {"root", "weapon_hand_L", "magazine_L", "attachWorld"},
        .Parents = {-1, 0, 1, 0},
        .Lod = 2,
    });
    const auto mod = Animation({.Names = {"root", "weapon_hand_l", "magazine_l"}, .Parents = {-1, 0, 0}, .Lod = 3});
    const auto edit = AnimationSkeletonMigration::Migrate(mod, kPath, game, kPath);
    EXPECT_EQ(edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(edit.Detail, "Animation override parents changed: magazine_L. Retargeting needs author review.");
}

TEST(Animation, UnsupportedMetadataBlocksARealEdit) {
    const auto game = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1});
    const auto unknown = Animation({.Names = {"arm", "root"}, .Parents = {1, -1}, .Lod = 1, .UnknownKey = "m_custom"});
    const auto unknown_edit = AnimationSkeletonMigration::Migrate(unknown, kPath, game, kPath);
    EXPECT_EQ(unknown_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(unknown_edit.Detail, "Animation override contains unsupported metadata or secondary skeletons.");

    const auto secondary = Animation({.Names = {"arm", "root"}, .Parents = {1, -1}, .Lod = 1, .SecondaryItem = true});
    const auto secondary_edit = AnimationSkeletonMigration::Migrate(secondary, kPath, game, kPath);
    EXPECT_EQ(secondary_edit.Outcome, ModelOutcome::NeedsReview);
    EXPECT_EQ(secondary_edit.Detail, "Animation override contains unsupported metadata or secondary skeletons.");

    const auto same = Animation({.Names = {"root", "arm"}, .Parents = {-1, 0}, .Lod = 1, .SecondaryItem = true});
    const auto same_edit = AnimationSkeletonMigration::Migrate(same, kPath, game, kPath);
    EXPECT_EQ(same_edit.Outcome, ModelOutcome::Current);
}
