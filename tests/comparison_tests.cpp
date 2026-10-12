#include "kv3.hpp"
#include "model_comparison.hpp"
#include "model_versions.hpp"
#include "preview_scene.hpp"
#include "preview_test_model.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using skeleton_repair::BindingDiff;
using skeleton_repair::BoneChange;
using skeleton_repair::BuildModelVersions;
using skeleton_repair::CompareBindings;
using skeleton_repair::CompareSkeletons;
using skeleton_repair::GeometryBone;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::ModelGeometry;
using skeleton_repair::ModelSources;
using skeleton_repair::ModelVersion;
using skeleton_repair::SkeletonDiff;

using Fields = std::vector<std::pair<std::string, KvValue>>;

GeometryBone Bone(const char* name, int parent, skeleton_repair::Vec3 position) {
    GeometryBone bone;
    bone.Name = name;
    bone.Parent = parent;
    bone.ModelPosition = position;
    return bone;
}

const BoneChange* ChangeOf(const SkeletonDiff& diff, const std::string& name) {
    for (const auto& bone : diff.Bones) {
        if (bone.Name == name) {
            return &bone.Change;
        }
    }
    return nullptr;
}

TEST(CompareSkeletons, ReorderedSkeletonIsIdentical) {
    const std::vector<GeometryBone> left = {Bone("root", -1, {0, 0, 0}), Bone("spine", 0, {0, 0, 10}), Bone("head", 1, {0, 0, 20})};
    const std::vector<GeometryBone> right = {Bone("root", -1, {0, 0, 0}), Bone("head", 2, {0, 0, 20}), Bone("spine", 0, {0, 0, 10})};
    const SkeletonDiff diff = CompareSkeletons(left, right);
    EXPECT_TRUE(diff.Identical());
    EXPECT_EQ(diff.Same, 3u);
}

TEST(CompareSkeletons, ClassifiesEveryKindOfChange) {
    const std::vector<GeometryBone> left = {Bone("root", -1, {0, 0, 0}), Bone("spine", 0, {0, 0, 10}), Bone("arm", 1, {5, 0, 15}),
        Bone("tail", 0, {-5, 0, 5})};
    const std::vector<GeometryBone> right = {Bone("root", -1, {0, 0, 0}), Bone("spine", 0, {0, 0, 12}), Bone("arm", 0, {5, 0, 15}),
        Bone("weapon", 2, {6, 0, 15})};
    const SkeletonDiff diff = CompareSkeletons(left, right);
    EXPECT_EQ(*ChangeOf(diff, "root"), BoneChange::Same);
    EXPECT_EQ(*ChangeOf(diff, "spine"), BoneChange::Moved);
    EXPECT_EQ(*ChangeOf(diff, "arm"), BoneChange::Reparented);
    EXPECT_EQ(*ChangeOf(diff, "weapon"), BoneChange::Added);
    EXPECT_EQ(*ChangeOf(diff, "tail"), BoneChange::Removed);
    EXPECT_EQ(diff.Same, 1u);
    EXPECT_EQ(diff.Moved, 1u);
    EXPECT_EQ(diff.Reparented, 1u);
    EXPECT_EQ(diff.Added, 1u);
    EXPECT_EQ(diff.Removed, 1u);
    for (const auto& bone : diff.Bones) {
        if (bone.Name == "spine") {
            EXPECT_FLOAT_EQ(bone.Distance, 2.0f);
        }
        if (bone.Name == "arm") {
            EXPECT_EQ(bone.LeftParent, "spine");
            EXPECT_EQ(bone.RightParent, "root");
        }
    }
}

TEST(CompareSkeletons, SmallDriftIsWithinTolerance) {
    const std::vector<GeometryBone> left = {Bone("root", -1, {0, 0, 0})};
    const std::vector<GeometryBone> right = {Bone("root", -1, {0.001f, 0, 0})};
    EXPECT_TRUE(CompareSkeletons(left, right).Identical());
}

// One skinned quad whose vertices use local bones 0 and 1.
ModelGeometry SkinnedQuad(std::vector<GeometryBone> bones, std::array<uint16_t, 4> first, std::array<uint16_t, 4> second) {
    ModelGeometry model;
    model.Meshes.push_back(preview_test::Quad("body", 10, 1, "m.vmat"));
    auto& vb = model.Meshes[0].VertexBuffers[0];
    vb.BoneIndices = {first, first, second, second};
    vb.BoneWeights = {{1, 0, 0, 0}, {1, 0, 0, 0}, {0.75f, 0.25f, 0, 0}, {0.75f, 0.25f, 0, 0}};
    model.Bones = std::move(bones);
    return model;
}

TEST(CompareBindings, RemappedIndicesKeepTheSameBones) {
    const ModelGeometry before = SkinnedQuad({Bone("root", -1, {}), Bone("spine", 0, {}), Bone("head", 1, {})}, {1, 0, 0, 0}, {2, 1, 0, 0});
    // The repair reorders the skeleton and rewrites indices to match.
    const ModelGeometry after = SkinnedQuad({Bone("head", 2, {}), Bone("root", -1, {}), Bone("spine", 1, {})}, {2, 0, 0, 0}, {0, 2, 0, 0});
    const BindingDiff diff = CompareBindings(before, after);
    ASSERT_TRUE(diff.Comparable);
    EXPECT_EQ(diff.Vertices, 4u);
    EXPECT_EQ(diff.Changed, 0u);
}

TEST(CompareBindings, DetectsVerticesMovedToOtherBones) {
    const ModelGeometry before = SkinnedQuad({Bone("root", -1, {}), Bone("spine", 0, {}), Bone("head", 1, {})}, {1, 0, 0, 0}, {2, 1, 0, 0});
    // Reordered skeleton but indices left alone: a broken remap.
    const ModelGeometry after = SkinnedQuad({Bone("head", 2, {}), Bone("root", -1, {}), Bone("spine", 1, {})}, {1, 0, 0, 0}, {2, 1, 0, 0});
    const BindingDiff diff = CompareBindings(before, after);
    ASSERT_TRUE(diff.Comparable);
    EXPECT_EQ(diff.Changed, 4u);
    ASSERT_FALSE(diff.Examples.empty());
    EXPECT_EQ(diff.Examples[0], "body: vertex 0");
}

TEST(CompareBindings, DifferentMeshesAreNotCompared) {
    ModelGeometry left = preview_test::TwoLodModel();
    ModelGeometry right = preview_test::TwoLodModel();
    right.Meshes[0].VertexBuffers[0].Positions[0].X = 3;
    const BindingDiff diff = CompareBindings(left, right);
    EXPECT_FALSE(diff.Comparable);
    EXPECT_NE(diff.Reason.find("vertex positions"), std::string::npos);
}

TEST(DiffColors, MarksChangesOnTheirSide) {
    const std::vector<GeometryBone> left = {Bone("root", -1, {}), Bone("tail", 0, {})};
    const std::vector<GeometryBone> right = {Bone("root", -1, {}), Bone("weapon", 0, {})};
    const SkeletonDiff diff = CompareSkeletons(left, right);
    EXPECT_EQ(skeleton_repair::DiffColors(left, diff, true),
        (std::vector<uint32_t>{skeleton_repair::kBoneLineColor, skeleton_repair::kRemovedBoneColor}));
    EXPECT_EQ(skeleton_repair::DiffColors(right, diff, false),
        (std::vector<uint32_t>{skeleton_repair::kBoneLineColor, skeleton_repair::kAddedBoneColor}));
}

TEST(BoneColors, VerticesTakeTheirStrongestBoneColor) {
    const ModelGeometry model = SkinnedQuad({Bone("root", -1, {}), Bone("spine", 0, {}), Bone("head", 1, {})}, {1, 0, 0, 0}, {2, 1, 0, 0});
    const auto scene = skeleton_repair::BuildPreviewScene(model);
    EXPECT_EQ(scene.Vertices[0].BoneColor, skeleton_repair::BoneNameColor("spine"));
    EXPECT_EQ(scene.Vertices[2].BoneColor, skeleton_repair::BoneNameColor("head"));
    EXPECT_NE(skeleton_repair::BoneNameColor("spine"), skeleton_repair::BoneNameColor("head"));

    const auto unskinned = skeleton_repair::BuildPreviewScene(preview_test::TwoLodModel());
    EXPECT_EQ(unskinned.Vertices[0].BoneColor, skeleton_repair::kUnskinnedColor);
}

// Minimal compiled resources: a DATA block only.
std::vector<uint8_t> Resource(KvValue root) {
    KvDocument document;
    document.Root = std::move(root);
    const std::vector<uint8_t> data = skeleton_repair::WriteKv3(document);
    std::vector<uint8_t> file(28);
    auto put = [&](size_t at, auto value) { std::memcpy(file.data() + at, &value, sizeof(value)); };
    put(4, uint16_t{12});
    put(8, uint32_t{8});
    put(12, int32_t{1});
    std::memcpy(file.data() + 16, "DATA", 4);
    put(20, uint32_t{8});
    put(24, static_cast<uint32_t>(data.size()));
    file.insert(file.end(), data.begin(), data.end());
    put(0, static_cast<uint32_t>(file.size()));
    return file;
}

KvValue Numbers(std::initializer_list<double> values) {
    std::vector<KvValue> items;
    for (double value : values) {
        items.push_back(KvValue::Float(static_cast<float>(value)));
    }
    return KvValue::Array(std::move(items));
}

std::vector<uint8_t> ModelFile(std::vector<std::string> names, const char* skeleton_ref) {
    std::vector<KvValue> name_values, parents, spheres, flags, positions, rotations, scales;
    for (size_t i = 0; i < names.size(); ++i) {
        name_values.push_back(KvValue::String(names[i]));
        parents.push_back(KvValue::Int32(i == 0 ? -1 : 0));
        spheres.push_back(Numbers({0, 0, 0, 1}));
        flags.push_back(KvValue::Int32(0));
        positions.push_back(Numbers({0, 0, static_cast<double>(i) * 10}));
        rotations.push_back(Numbers({0, 0, 0, 1}));
        scales.push_back(KvValue::Float(1));
    }
    KvValue skeleton = KvValue::Object(Fields{
        {"m_boneName", KvValue::Array(std::move(name_values))},
        {"m_nParent", KvValue::Array(std::move(parents))},
        {"m_boneSphere", KvValue::Array(std::move(spheres))},
        {"m_nFlag", KvValue::Array(std::move(flags))},
        {"m_bonePosParent", KvValue::Array(std::move(positions))},
        {"m_boneRotParent", KvValue::Array(std::move(rotations))},
        {"m_boneScaleParent", KvValue::Array(std::move(scales))},
    });
    Fields fields{{"m_name", KvValue::String("models/heroes/test/test.vmdl")}, {"m_modelSkeleton", std::move(skeleton)}};
    if (skeleton_ref != nullptr) {
        fields.emplace_back("m_vecNmSkeletonRefs", KvValue::Array({KvValue::String(skeleton_ref)}));
    }
    return Resource(KvValue::Object(std::move(fields)));
}

std::vector<uint8_t> AnimationSkeletonFile(std::vector<std::string> names) {
    std::vector<KvValue> ids, parents, poses;
    for (size_t i = 0; i < names.size(); ++i) {
        ids.push_back(KvValue::String(names[i]));
        parents.push_back(KvValue::Int32(i == 0 ? -1 : 0));
        poses.push_back(Numbers({0, 0, static_cast<double>(i) * 10, 1, 0, 0, 0, 1}));
    }
    std::vector<KvValue> model_poses = poses;
    return Resource(KvValue::Object(Fields{
        {"m_ID", KvValue::String("models/heroes/test/test.vnmskel")},
        {"m_boneIDs", KvValue::Array(std::move(ids))},
        {"m_parentIndices", KvValue::Array(std::move(parents))},
        {"m_parentSpaceReferencePose", KvValue::Array(std::move(poses))},
        {"m_modelSpaceReferencePose", KvValue::Array(std::move(model_poses))},
        {"m_numBonesToSampleAtLowLOD", KvValue::Int32(static_cast<int32_t>(names.size()))},
    }));
}

constexpr const char* kModelPath = "models/heroes/test/test.vmdl_c";

TEST(ModelVersions, FindsTheAnimationSkeletonFile) {
    const ModelGeometry model = skeleton_repair::DecodeModelGeometry(ModelFile({"root"}, "models/heroes/Test/test.vnmskel"));
    // Case is kept; archive lookups ignore case.
    EXPECT_EQ(skeleton_repair::AnimationSkeletonPath(model), std::optional<std::string>("models/heroes/Test/test.vnmskel_c"));
}

TEST(ModelVersions, WithoutTheGameOnlyTheModIsAvailable) {
    ModelSources sources;
    sources.ModelPath = kModelPath;
    sources.ModModel = ModelFile({"root", "spine"}, nullptr);
    const auto versions = BuildModelVersions(sources);
    EXPECT_TRUE(versions.Get(ModelVersion::Mod).Available);
    ASSERT_TRUE(versions.Get(ModelVersion::Mod).Model);
    EXPECT_EQ(versions.Get(ModelVersion::Mod).Model->Bones.size(), 2u);
    EXPECT_FALSE(versions.Get(ModelVersion::Repaired).Available);
    EXPECT_FALSE(versions.Get(ModelVersion::Game).Available);
    EXPECT_FALSE(versions.ModelEdit);
}

TEST(ModelVersions, CurrentModelKeepsTheModGeometryAsRepaired) {
    ModelSources sources;
    sources.ModelPath = kModelPath;
    sources.ModModel = ModelFile({"root", "spine"}, nullptr);
    sources.GameModelPath = kModelPath;
    sources.GameModel = ModelFile({"root", "spine"}, nullptr);
    const auto versions = BuildModelVersions(sources);
    ASSERT_TRUE(versions.ModelEdit);
    EXPECT_FALSE(versions.ModelEdit->Bytes);
    const auto& repaired = versions.Get(ModelVersion::Repaired);
    ASSERT_TRUE(repaired.Available);
    ASSERT_TRUE(repaired.Model);
    EXPECT_EQ(repaired.Model->Bones.size(), 2u);
    ASSERT_FALSE(repaired.Notes.empty());
    EXPECT_EQ(repaired.Notes[0].rfind("Model unchanged.", 0), 0u);
}

TEST(ModelVersions, ModWithoutSkeletonOverrideUsesTheGameSkeleton) {
    ModelSources sources;
    sources.ModelPath = kModelPath;
    sources.ModModel = ModelFile({"root", "spine"}, "models/heroes/test/test.vnmskel");
    sources.GameModelPath = kModelPath;
    sources.GameModel = ModelFile({"root", "spine"}, "models/heroes/test/test.vnmskel");
    sources.SkeletonPath = "models/heroes/test/test.vnmskel_c";
    sources.GameSkeleton = AnimationSkeletonFile({"root", "spine", "weapon"});
    const auto versions = BuildModelVersions(sources);
    const auto& mod = versions.Get(ModelVersion::Mod);
    ASSERT_TRUE(mod.AnimationSkeleton);
    EXPECT_EQ(mod.AnimationSkeleton->size(), 3u);
    EXPECT_FALSE(versions.SkeletonEdit);
    ASSERT_TRUE(versions.Get(ModelVersion::Game).AnimationSkeleton);
}

TEST(ModelVersions, SkeletonOverrideRunsTheAnimationMigration) {
    ModelSources sources;
    sources.ModelPath = kModelPath;
    sources.ModModel = ModelFile({"root", "spine"}, "models/heroes/test/test.vnmskel");
    sources.GameModelPath = kModelPath;
    sources.GameModel = ModelFile({"root", "spine"}, "models/heroes/test/test.vnmskel");
    sources.SkeletonPath = "models/heroes/test/test.vnmskel_c";
    sources.ModSkeleton = AnimationSkeletonFile({"root", "spine"});
    sources.GameSkeleton = AnimationSkeletonFile({"root", "spine", "weapon"});
    const auto versions = BuildModelVersions(sources);
    ASSERT_TRUE(versions.SkeletonEdit);
    // Adding the game-only bone is a supported rebind, so the repaired skeleton has it.
    EXPECT_EQ(versions.SkeletonEdit->Outcome, skeleton_repair::ModelOutcome::Updated);
    ASSERT_TRUE(versions.SkeletonEdit->Bytes);
    const auto& repaired = versions.Get(ModelVersion::Repaired);
    ASSERT_TRUE(repaired.AnimationSkeleton);
    ASSERT_EQ(repaired.AnimationSkeleton->size(), 3u);
    EXPECT_EQ((*repaired.AnimationSkeleton)[2].Name, "weapon");
    // The mod side still shows the skeleton as shipped.
    EXPECT_EQ(versions.Get(ModelVersion::Mod).AnimationSkeleton->size(), 2u);
    EXPECT_GE(repaired.Notes.size(), 2u);
}

}  // namespace
