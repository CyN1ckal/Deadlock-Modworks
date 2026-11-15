#include "camera_attachment.hpp"
#include "kv3.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using skeleton_repair::CameraAttachmentEdit;
using skeleton_repair::CameraAttachmentRepair;
using skeleton_repair::CameraFrame;
using skeleton_repair::KvDocument;
using skeleton_repair::KvKind;
using skeleton_repair::KvValue;
using skeleton_repair::ReadKv3;
using skeleton_repair::WriteKv3;

using Fields = std::vector<std::pair<std::string, KvValue>>;

constexpr double kHalfSqrt2 = 0.7071067811865476;

template <typename T>
void Put(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

KvValue Floats(std::initializer_list<double> values) {
    std::vector<KvValue> items;
    for (double value : values) {
        items.push_back(KvValue::Float(static_cast<float>(value)));
    }
    return KvValue::Array(std::move(items));
}

KvValue Q(double x, double y, double z, double w) {
    return Floats({x, y, z, w});
}

KvValue V(double x, double y, double z) {
    return Floats({x, y, z});
}

KvValue Names(std::initializer_list<const char*> values) {
    std::vector<KvValue> items;
    for (const char* value : values) {
        items.push_back(KvValue::String(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Bone(const char* name, KvValue inverse_bind) {
    return KvValue::Object(Fields{
        {"m_boneName", KvValue::String(name)},
        {"m_invBindPose", std::move(inverse_bind)},
    });
}

KvValue FlatBind() {
    return Floats({0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0});
}

KvValue IdentityBind() {
    return Floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
}

KvValue NestedBind() {
    return KvValue::Array({
        Floats({0, 1, 0, 0}),
        Floats({0, 0, 1, 0}),
        Floats({1, 0, 0, 0}),
    });
}

// Three slots. Only the first `live` entries are influences; the rest are padding.
KvValue Attachment(const char* name, int live, std::vector<const char*> bones, std::vector<KvValue> rotations, std::vector<KvValue> offsets) {
    return KvValue::Object(Fields{
        {"m_name", KvValue::String(name)},
        {"m_influenceNames", Names({bones[0], bones.size() > 1 ? bones[1] : "", bones.size() > 2 ? bones[2] : ""})},
        {"m_vInfluenceRotations", KvValue::Array(std::move(rotations))},
        {"m_vInfluenceOffsets", KvValue::Array(std::move(offsets))},
        {"m_influenceWeights", Floats({1.0, 0.25, 0.5})},
        {"m_nInfluences", KvValue::Int64(live)},
    });
}

std::vector<KvValue> PadQuat(KvValue first, KvValue second = Q(0, 0, 0, 1), KvValue third = Q(0, 0, 0, 1)) {
    std::vector<KvValue> values;
    values.push_back(std::move(first));
    values.push_back(std::move(second));
    values.push_back(std::move(third));
    return values;
}

std::vector<KvValue> PadVec(KvValue first, KvValue second = V(0, 0, 0), KvValue third = V(0, 0, 0)) {
    std::vector<KvValue> values;
    values.push_back(std::move(first));
    values.push_back(std::move(second));
    values.push_back(std::move(third));
    return values;
}

KvValue MeshRoot(KvValue inverse_bind, std::vector<KvValue> attachments) {
    const KvValue pelvis = Bone("pelvis", IdentityBind());
    const KvValue motion = Bone("root_motion", std::move(inverse_bind));
    return KvValue::Object(Fields{
        {"m_skeleton", KvValue::Object(Fields{{"m_bones", KvValue::Array({pelvis, motion})}})},
        {"m_attachments", KvValue::Array(std::move(attachments))},
    });
}

std::vector<uint8_t> Kv(KvValue root) {
    KvDocument document;
    document.Root = std::move(root);
    return WriteKv3(document);
}

std::vector<uint8_t> Resource(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& blocks) {
    const size_t table = 16;
    size_t at = table + blocks.size() * 12;
    std::vector<uint8_t> file(at);
    for (const auto& block : blocks) {
        file.insert(file.end(), block.second.begin(), block.second.end());
    }
    Put<uint32_t>(file, 0, static_cast<uint32_t>(file.size()));
    Put<uint16_t>(file, 4, 12);
    Put<uint16_t>(file, 6, 0);
    Put<uint32_t>(file, 8, 8);
    Put<int32_t>(file, 12, static_cast<int32_t>(blocks.size()));
    for (size_t i = 0; i < blocks.size(); ++i) {
        const size_t pos = table + i * 12;
        std::memcpy(file.data() + pos, blocks[i].first.data(), 4);
        Put<uint32_t>(file, pos + 4, static_cast<uint32_t>(at - (pos + 4)));
        Put<uint32_t>(file, pos + 8, static_cast<uint32_t>(blocks[i].second.size()));
        at += blocks[i].second.size();
    }
    return file;
}

std::vector<KvDocument> CameraMeshes(const std::vector<uint8_t>& file) {
    uint32_t block_offset = 0;
    int32_t count = 0;
    std::memcpy(&block_offset, file.data() + 8, 4);
    std::memcpy(&count, file.data() + 12, 4);
    const size_t table = 8u + block_offset;
    std::vector<KvDocument> meshes;
    for (int i = 0; i < count; ++i) {
        const size_t pos = table + static_cast<size_t>(i) * 12;
        if (std::memcmp(file.data() + pos, "MDAT", 4) != 0) {
            continue;
        }
        uint32_t relative = 0;
        int32_t size = 0;
        std::memcpy(&relative, file.data() + pos + 4, 4);
        std::memcpy(&size, file.data() + pos + 8, 4);
        const size_t absolute = pos + 4 + relative;
        KvDocument document = ReadKv3(std::span<const uint8_t>(file.data() + absolute, static_cast<size_t>(size)));
        if (document.Root.Contains("m_attachments")) {
            meshes.push_back(std::move(document));
        }
    }
    return meshes;
}

const KvValue& Body(const KvDocument& document, const char* name) {
    const KvValue* attachments = document.Root.Find("m_attachments");
    EXPECT_NE(attachments, nullptr);
    for (const KvValue& item : attachments->Items()) {
        std::string found;
        if (const KvValue* value = item.Find("value"); value != nullptr && value->Find("m_name") && value->Find("m_name")->Kind == KvKind::String) {
            found = value->Find("m_name")->String();
        } else if (const KvValue* key = item.Find("key"); key != nullptr && key->Kind == KvKind::String) {
            found = key->String();
        } else if (const KvValue* own = item.Find("m_name"); own != nullptr && own->Kind == KvKind::String) {
            found = own->String();
        }
        if (found != name) {
            continue;
        }
        if (const KvValue* value = item.Find("value"); value != nullptr && value->Contains("m_influenceNames")) {
            return *value;
        }
        return item;
    }
    ADD_FAILURE() << name;
    return document.Root;
}

void ExpectNumbers(const KvValue& value, std::initializer_list<double> expected, double tolerance = 1e-5) {
    ASSERT_TRUE(value.IsArray());
    ASSERT_EQ(value.Count(), expected.size());
    size_t index = 0;
    for (double number : expected) {
        EXPECT_NEAR(value.At(index).ToDouble(), number, tolerance) << index;
        ++index;
    }
}

const KvValue& Slot(const KvValue& body, const char* field, int slot) {
    const KvValue* values = body.Find(field);
    EXPECT_NE(values, nullptr);
    return values->At(static_cast<size_t>(slot));
}

std::vector<uint8_t> OldAndCurrentFile() {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5), Q(0.2, 0.3, 0.4, 0.5)),
        PadVec(V(0, 85, 0), V(9, 8, 7)));
    const KvValue crouching = Attachment("crouching_pivot", 1, {"root_motion"},
        PadQuat(Q(0.5, 0.5, 0.5, -0.5)),
        PadVec(V(0, 60, 0)));
    const KvValue aim = Attachment("aim", 2, {"root_motion", "head"},
        PadQuat(Q(0, -kHalfSqrt2, 0, kHalfSqrt2), Q(0, 0, 0, 1)),
        PadVec(V(0, 10, 3), V(1, 2, 3)));
    const KvValue near = Attachment("near_00", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5)),
        PadVec(V(-30, 85, -14)));
    KvValue portrait = Attachment("portrait_camera", 1, {"root_motion"},
        PadQuat(Q(-0.58479, 0.39347, 0.38835, 0.59362)),
        PadVec(V(-83.27528, 24.62402, 221.85091)));
    portrait.Fields().erase(portrait.Fields().begin());
    const KvValue wrapped = KvValue::Object(Fields{
        {"key", KvValue::String("portrait_camera")},
        {"value", std::move(portrait)},
    });
    const KvValue muzzle = Attachment("muzzle", 1, {"head"},
        PadQuat(Q(0, 0, 0, 1)),
        PadVec(V(1, 2, 3)));

    const KvValue current_standing = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(0, 0, 0, 1)),
        PadVec(V(0, 0, 86)));
    const KvValue current_aim = Attachment("aim", 1, {"root_motion"},
        PadQuat(Q(0.2, 0.3, 0.4, 0.5)),
        PadVec(V(4, 5, 6)));

    return Resource({
        {"DATA", Kv(KvValue::Object({}))},
        {"MDAT", Kv(MeshRoot(FlatBind(), {standing, crouching, aim, near, wrapped, muzzle}))},
        {"MDAT", Kv(MeshRoot(FlatBind(), {current_standing, current_aim}))},
        {"PHYS", {1, 2, 3, 4}},
    });
}

}  // namespace

TEST(CameraAttachment, OldRootMotionFrameIsRebasedAndACurrentMeshIsLeftAlone) {
    const std::vector<uint8_t> file = OldAndCurrentFile();
    const CameraAttachmentEdit seen = CameraAttachmentRepair::Inspect(file);
    EXPECT_EQ(seen.Frame, CameraFrame::Old);
    EXPECT_EQ(seen.Meshes, 1);
    EXPECT_EQ(seen.Influences, 5);
    EXPECT_FALSE(seen.Bytes.has_value());
    EXPECT_NE(seen.Detail.find("October 16, 2026"), std::string::npos);
    EXPECT_NE(seen.Detail.find("5 influences on 1 mesh"), std::string::npos);
    EXPECT_NE(seen.Detail.find("mod's own offsets are kept"), std::string::npos);

    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    ASSERT_EQ(edit.Frame, CameraFrame::Old);
    ASSERT_TRUE(edit.Bytes.has_value());
    EXPECT_EQ(edit.Influences, 5);
    uint32_t stored_size = 0;
    std::memcpy(&stored_size, edit.Bytes->data(), 4);
    EXPECT_EQ(stored_size, edit.Bytes->size());
    EXPECT_EQ(std::vector<uint8_t>(edit.Bytes->end() - 4, edit.Bytes->end()), (std::vector<uint8_t>{1, 2, 3, 4}));

    const std::vector<KvDocument> meshes = CameraMeshes(*edit.Bytes);
    ASSERT_EQ(meshes.size(), 2u);
    const KvValue& standing = Body(meshes[0], "standing_pivot");
    ExpectNumbers(Slot(standing, "m_vInfluenceRotations", 0), {0, 0, 0, 1});
    ExpectNumbers(Slot(standing, "m_vInfluenceOffsets", 0), {0, 0, 85});
    ExpectNumbers(Slot(standing, "m_vInfluenceRotations", 1), {0.2, 0.3, 0.4, 0.5});
    ExpectNumbers(Slot(standing, "m_vInfluenceOffsets", 1), {9, 8, 7});
    ExpectNumbers(*standing.Find("m_influenceWeights"), {1, 0.25, 0.5});
    EXPECT_EQ(standing.Find("m_nInfluences")->Kind, KvKind::Int64);
    EXPECT_EQ(standing.Find("m_nInfluences")->ToInt32(), 1);

    const KvValue& crouching = Body(meshes[0], "crouching_pivot");
    ExpectNumbers(Slot(crouching, "m_vInfluenceRotations", 0), {0, 0, 0, 1});
    ExpectNumbers(Slot(crouching, "m_vInfluenceOffsets", 0), {0, 0, 60});

    const KvValue& aim = Body(meshes[0], "aim");
    ExpectNumbers(Slot(aim, "m_vInfluenceRotations", 0), {kHalfSqrt2, 0, 0, kHalfSqrt2});
    ExpectNumbers(Slot(aim, "m_vInfluenceOffsets", 0), {3, 0, 10});
    ExpectNumbers(Slot(aim, "m_vInfluenceRotations", 1), {0, 0, 0, 1});
    ExpectNumbers(Slot(aim, "m_vInfluenceOffsets", 1), {1, 2, 3});
    EXPECT_EQ(aim.Find("m_nInfluences")->ToInt32(), 2);

    const KvValue& near = Body(meshes[0], "near_00");
    ExpectNumbers(Slot(near, "m_vInfluenceOffsets", 0), {-14, -30, 85});

    const KvValue& portrait = Body(meshes[0], "portrait_camera");
    ExpectNumbers(Slot(portrait, "m_vInfluenceRotations", 0), {0.00185, 0.00697, 0.98011, 0.19830}, 1e-3);
    ExpectNumbers(Slot(portrait, "m_vInfluenceOffsets", 0), {221.85091, -83.27528, 24.62402}, 1e-3);

    const KvValue& muzzle = Body(meshes[0], "muzzle");
    ExpectNumbers(Slot(muzzle, "m_vInfluenceRotations", 0), {0, 0, 0, 1});
    ExpectNumbers(Slot(muzzle, "m_vInfluenceOffsets", 0), {1, 2, 3});
    EXPECT_EQ(muzzle.Find("m_influenceNames")->At(0).String(), "head");

    const KvValue& current_standing = Body(meshes[1], "standing_pivot");
    ExpectNumbers(Slot(current_standing, "m_vInfluenceRotations", 0), {0, 0, 0, 1});
    ExpectNumbers(Slot(current_standing, "m_vInfluenceOffsets", 0), {0, 0, 86});
    const KvValue& current_aim = Body(meshes[1], "aim");
    ExpectNumbers(Slot(current_aim, "m_vInfluenceRotations", 0), {0.2, 0.3, 0.4, 0.5});
    ExpectNumbers(Slot(current_aim, "m_vInfluenceOffsets", 0), {4, 5, 6});

    const CameraAttachmentEdit again = CameraAttachmentRepair::Apply(*edit.Bytes);
    EXPECT_EQ(again.Frame, CameraFrame::Current);
    EXPECT_FALSE(again.Bytes.has_value());
    EXPECT_TRUE(again.Detail.empty());
}

TEST(CameraAttachment, TwoOldMeshesKeepTheTrailingBlock) {
    const auto standing = [](double height) {
        return Attachment("standing_pivot", 1, {"root_motion"}, PadQuat(Q(-0.5, -0.5, -0.5, 0.5)), PadVec(V(0, height, 0)));
    };
    const std::vector<uint8_t> file = Resource({
        {"DATA", Kv(KvValue::Object({}))},
        {"MDAT", Kv(MeshRoot(FlatBind(), {standing(85)}))},
        {"MDAT", Kv(MeshRoot(FlatBind(), {standing(60)}))},
        {"PHYS", {1, 2, 3, 4}},
    });
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    ASSERT_TRUE(edit.Bytes.has_value());
    EXPECT_EQ(edit.Meshes, 2);
    EXPECT_EQ(edit.Influences, 2);
    EXPECT_NE(edit.Detail.find("2 influences on 2 meshes"), std::string::npos);
    EXPECT_EQ(std::vector<uint8_t>(edit.Bytes->end() - 4, edit.Bytes->end()), (std::vector<uint8_t>{1, 2, 3, 4}));
    const std::vector<KvDocument> meshes = CameraMeshes(*edit.Bytes);
    ASSERT_EQ(meshes.size(), 2u);
    ExpectNumbers(Slot(Body(meshes[0], "standing_pivot"), "m_vInfluenceOffsets", 0), {0, 0, 85});
    ExpectNumbers(Slot(Body(meshes[1], "standing_pivot"), "m_vInfluenceOffsets", 0), {0, 0, 60});
}

TEST(CameraAttachment, NestedInverseBindUsesTheSameFrame) {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5)), PadVec(V(0, 85, 0)));
    KvValue attachment = standing;
    attachment.Set("m_nInfluences", KvValue::UInt32(1));
    const std::vector<uint8_t> file = Resource({{"MDAT", Kv(MeshRoot(NestedBind(), {attachment}))}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    ASSERT_TRUE(edit.Bytes.has_value());
    const std::vector<KvDocument> meshes = CameraMeshes(*edit.Bytes);
    ASSERT_EQ(meshes.size(), 1u);
    const KvValue& pivot = Body(meshes[0], "standing_pivot");
    ExpectNumbers(Slot(pivot, "m_vInfluenceRotations", 0), {0, 0, 0, 1});
    ExpectNumbers(Slot(pivot, "m_vInfluenceOffsets", 0), {0, 0, 85});
    EXPECT_EQ(pivot.Find("m_nInfluences")->Kind, KvKind::UInt32);
}

TEST(CameraAttachment, IdentityPivotIsCurrent) {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"}, PadQuat(Q(0, 0, 0, 1)), PadVec(V(0, 0, 85)));
    const std::vector<uint8_t> file = Resource({{"MDAT", Kv(MeshRoot(FlatBind(), {standing}))}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    EXPECT_EQ(edit.Frame, CameraFrame::Current);
    EXPECT_FALSE(edit.Bytes.has_value());
    EXPECT_TRUE(edit.Detail.empty());
}

TEST(CameraAttachment, IdentityBindIsNotRewritten) {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"}, PadQuat(Q(0, 0, 0, 1)), PadVec(V(0, 85, 0)));
    const std::vector<uint8_t> current = Resource({{"MDAT", Kv(MeshRoot(IdentityBind(), {standing}))}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(current);
    EXPECT_EQ(edit.Frame, CameraFrame::Current);
    EXPECT_FALSE(edit.Bytes.has_value());

    const KvValue old_numbers = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5)), PadVec(V(0, 85, 0)));
    const std::vector<uint8_t> other = Resource({{"MDAT", Kv(MeshRoot(IdentityBind(), {old_numbers}))}});
    const CameraAttachmentEdit unrecognized = CameraAttachmentRepair::Apply(other);
    EXPECT_EQ(unrecognized.Frame, CameraFrame::Unrecognized);
    EXPECT_FALSE(unrecognized.Bytes.has_value());
}

TEST(CameraAttachment, UnknownPivotIsNotWritten) {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(0, 0, kHalfSqrt2, kHalfSqrt2)), PadVec(V(0, 85, 0)));
    const std::vector<uint8_t> file = Resource({{"MDAT", Kv(MeshRoot(FlatBind(), {standing}))}, {"PHYS", {1, 2, 3, 4}}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    EXPECT_EQ(edit.Frame, CameraFrame::Unrecognized);
    EXPECT_FALSE(edit.Bytes.has_value());
    EXPECT_NE(edit.Detail.find("left unchanged"), std::string::npos);
}

TEST(CameraAttachment, FlatRotationListIsNotWritten) {
    KvValue standing = KvValue::Object(Fields{
        {"m_name", KvValue::String("standing_pivot")},
        {"m_influenceNames", Names({"root_motion"})},
        {"m_vInfluenceRotations", Floats({-0.5, -0.5, -0.5, 0.5})},
        {"m_vInfluenceOffsets", Floats({0, 85, 0})},
        {"m_nInfluences", KvValue::Int32(1)},
    });
    const std::vector<uint8_t> file = Resource({{"MDAT", Kv(MeshRoot(FlatBind(), {standing}))}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    EXPECT_EQ(edit.Frame, CameraFrame::Unrecognized);
    EXPECT_FALSE(edit.Bytes.has_value());
}

TEST(CameraAttachment, DisagreeingPivotsAreNotWritten) {
    const KvValue standing = Attachment("standing_pivot", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5)), PadVec(V(0, 85, 0)));
    KvValue crouching = Attachment("crouching_pivot", 1, {"root_motion"},
        PadQuat(Q(0, 0, 0, 1)), PadVec(V(0, 0, 60)));
    crouching.Fields().erase(crouching.Fields().begin());
    const KvValue wrapped = KvValue::Object(Fields{{"key", KvValue::String("crouching_pivot")}, {"value", std::move(crouching)}});
    const std::vector<uint8_t> file = Resource({{"MDAT", Kv(MeshRoot(FlatBind(), {standing, wrapped}))}});
    const CameraAttachmentEdit edit = CameraAttachmentRepair::Apply(file);
    EXPECT_EQ(edit.Frame, CameraFrame::Unrecognized);
    EXPECT_FALSE(edit.Bytes.has_value());
}

TEST(CameraAttachment, NoRootMotionPivotIsNone) {
    const std::vector<uint8_t> empty = Resource({{"DATA", Kv(KvValue::Object({}))}});
    const CameraAttachmentEdit none = CameraAttachmentRepair::Apply(empty);
    EXPECT_EQ(none.Frame, CameraFrame::None);
    EXPECT_FALSE(none.Bytes.has_value());
    EXPECT_TRUE(none.Detail.empty());

    const KvValue hat = Attachment("hat", 1, {"root_motion"},
        PadQuat(Q(-0.5, -0.5, -0.5, 0.5)), PadVec(V(0, 85, 0)));
    const std::vector<uint8_t> other = Resource({{"MDAT", Kv(MeshRoot(FlatBind(), {hat}))}});
    const CameraAttachmentEdit skipped = CameraAttachmentRepair::Apply(other);
    EXPECT_EQ(skipped.Frame, CameraFrame::None);
    EXPECT_FALSE(skipped.Bytes.has_value());
}
