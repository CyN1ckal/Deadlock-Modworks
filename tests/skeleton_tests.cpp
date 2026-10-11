#include "error.hpp"
#include "report.hpp"
#include "skeleton.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

skeleton_repair::BoneShape Bone(std::string name, std::optional<std::string> parent = std::nullopt) {
    return skeleton_repair::BoneShape{std::move(name), std::move(parent), {}, {0, 0, 0, 1}, 1};
}

using skeleton_repair::BoneNameMapping;
using skeleton_repair::ModelOutcome;
using skeleton_repair::ModelResult;
using skeleton_repair::SkeletonCompatibility;
using skeleton_repair::UpdateReport;

}  // namespace

TEST(Skeleton, ReorderingIsRepairable) {
    const auto arm = Bone("arm", "root");
    const auto root = Bone("root");
    const auto assessment = SkeletonCompatibility::Assess({arm, root}, {root, arm});
    EXPECT_FALSE(assessment.NeedsReview);
    EXPECT_EQ(assessment.Detail, "Game-only bones: 0; mod-only bones: 0.");
}

TEST(Skeleton, AdditionsWithoutAmbiguousOldBonesAreSupported) {
    const auto root = Bone("root");
    const auto arm = Bone("arm", "root");
    EXPECT_FALSE(SkeletonCompatibility::Assess({root}, {root, arm}).NeedsReview);
}

TEST(Skeleton, CustomHelperIsPreserved) {
    const auto root = Bone("root");
    const auto arm = Bone("arm", "root");
    EXPECT_FALSE(SkeletonCompatibility::Assess({root, arm}, {root}).NeedsReview);
}

TEST(Skeleton, PotentialRenameNeedsReview) {
    const auto root = Bone("root");
    const auto arm = Bone("arm", "root");
    const auto assessment = SkeletonCompatibility::Assess({root, arm}, {root, Bone("new_arm", "root")});
    EXPECT_TRUE(assessment.NeedsReview);
    EXPECT_NE(assessment.Detail.find("cannot be inferred safely"), std::string::npos);
}

TEST(Skeleton, ParentChangeNeedsReviewEvenWhenNamesMatch) {
    const auto root = Bone("root");
    auto game_arm = Bone("arm", "root");
    game_arm.Parent = std::nullopt;
    const auto assessment = SkeletonCompatibility::Assess({root, Bone("arm", "root")}, {root, game_arm});
    EXPECT_TRUE(assessment.NeedsReview);
    EXPECT_NE(assessment.Detail.find("Authored hierarchy differs"), std::string::npos);
}

TEST(Skeleton, ExternalSkeletonKeepsParentChangesAsNotes) {
    const auto root = Bone("root");
    auto game_arm = Bone("arm", "root");
    game_arm.Parent = std::nullopt;
    const auto assessment = SkeletonCompatibility::Assess({root, Bone("arm", "root")}, {root, game_arm}, true);
    EXPECT_FALSE(assessment.NeedsReview);
    ASSERT_FALSE(assessment.Warnings.empty());
    EXPECT_NE(assessment.Warnings[0].find("Authored hierarchy differs"), std::string::npos);
}

TEST(Skeleton, CustomRestPosesAreNonBlockingNotes) {
    const auto root = Bone("root");
    auto posed = Bone("arm", "root");
    posed.Position = {1, 0, 0};
    const auto assessment = SkeletonCompatibility::Assess({root, Bone("arm", "root")}, {root, posed});
    EXPECT_FALSE(assessment.NeedsReview);
    ASSERT_EQ(assessment.Warnings.size(), 1u);
    EXPECT_EQ(assessment.Warnings[0],
        "Custom rest poses (1): arm. Mod transforms are preserved; this is not evidence of a broken mod.");
}

TEST(Skeleton, EquivalentQuaternionSignsMatch) {
    auto flipped = Bone("root");
    flipped.Rotation = {0, 0, 0, -1};
    const auto assessment = SkeletonCompatibility::Assess({Bone("root")}, {flipped});
    EXPECT_FALSE(assessment.NeedsReview);
    EXPECT_TRUE(assessment.Warnings.empty());
}

TEST(Skeleton, CustomScaleIsANonBlockingNote) {
    auto scaled = Bone("root");
    scaled.Scale = 2;
    const auto assessment = SkeletonCompatibility::Assess({Bone("root")}, {scaled});
    EXPECT_FALSE(assessment.NeedsReview);
    EXPECT_FALSE(assessment.Warnings.empty());
}

TEST(Skeleton, DuplicateBonesFail) {
    const auto root = Bone("root");
    EXPECT_THROW(SkeletonCompatibility::Assess({root, root}, {root}), skeleton_repair::InvalidData);
}

TEST(Skeleton, MissingParentFails) {
    EXPECT_THROW(SkeletonCompatibility::Assess({Bone("arm", "root")}, {Bone("root")}), skeleton_repair::InvalidData);
}

TEST(Skeleton, ParentCycleFails) {
    auto cycled = Bone("root");
    cycled.Parent = "arm";
    EXPECT_THROW(SkeletonCompatibility::Assess({cycled, Bone("arm", "root")}, {Bone("root")}), skeleton_repair::InvalidData);
}

TEST(Skeleton, NonFiniteTransformFails) {
    auto broken = Bone("root");
    broken.Scale = std::nanf("");
    EXPECT_THROW(SkeletonCompatibility::Assess({broken}, {Bone("root")}), skeleton_repair::InvalidData);
}

TEST(Skeleton, ReviewBlocksAnOtherwiseRepairablePackage) {
    const UpdateReport report{{
        ModelResult{"a", ModelOutcome::Repairable, ""},
        ModelResult{"b", ModelOutcome::NeedsReview, ""},
    }, false, std::nullopt, std::nullopt};
    EXPECT_FALSE(report.CanRepair());
}

TEST(Skeleton, UnsupportedModsAreNotCalledCompatible) {
    const UpdateReport report{{ModelResult{"a", ModelOutcome::Skipped, ""}}, false, std::nullopt, std::nullopt};
    EXPECT_TRUE(report.HasBlockers());
}

TEST(BoneNames, UniqueCaseChangeIsAnAlias) {
    const auto mapping = BoneNameMapping::Resolve({"Arm"}, {"arm"});
    ASSERT_EQ(mapping.Aliases.size(), 1u);
    EXPECT_EQ(mapping.Aliases[0].first, "Arm");
    EXPECT_EQ(mapping.Aliases[0].second, "arm");
    EXPECT_TRUE(mapping.Unmatched.empty());
    EXPECT_TRUE(mapping.Ambiguous.empty());
}

TEST(BoneNames, SemanticRenameIsNotGuessed) {
    const auto mapping = BoneNameMapping::Resolve({"arm_l"}, {"arm_r"});
    EXPECT_TRUE(mapping.Aliases.empty());
    EXPECT_EQ(mapping.Unmatched, std::vector<std::string>{"arm_l"});
    EXPECT_TRUE(mapping.Ambiguous.empty());
}

TEST(BoneNames, CollidingCaseMatchesAreAmbiguous) {
    const auto mapping = BoneNameMapping::Resolve({"Arm"}, {"arm", "ARM"});
    EXPECT_TRUE(mapping.Aliases.empty());
    EXPECT_EQ(mapping.Unmatched, std::vector<std::string>{"Arm"});
    EXPECT_EQ(mapping.Ambiguous, std::vector<std::string>{"Arm"});
}
