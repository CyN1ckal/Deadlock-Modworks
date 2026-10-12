#include "error.hpp"
#include "kv3.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::vector<uint8_t> Load(const char* name) {
    const auto path = std::filesystem::path(SR_SOURCE_DIR) / "tests" / "data" / name;
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path.string();
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void ExpectBytes(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected) {
    const size_t count = actual.size() < expected.size() ? actual.size() : expected.size();
    for (size_t i = 0; i < count; ++i) {
        if (actual[i] != expected[i]) {
            ADD_FAILURE() << "byte " << i << " is " << static_cast<int>(actual[i]) << " expected " << static_cast<int>(expected[i])
                          << " (sizes " << actual.size() << " and " << expected.size() << ")";
            return;
        }
    }
    if (actual.size() != expected.size()) {
        ADD_FAILURE() << "size " << actual.size() << " expected " << expected.size() << " and the common prefix matches";
    }
}

skeleton_repair::KvValue Scalars() {
    using namespace skeleton_repair;
    std::vector<std::pair<std::string, KvValue>> fields;
    auto add = [&](std::string key, KvValue value) { fields.emplace_back(std::move(key), std::move(value)); };
    add("null", KvValue::Null());
    add("yes", KvValue::Bool(true));
    add("no", KvValue::Bool(false));
    add("i32", KvValue::Int32(-2));
    add("u32", KvValue::UInt32(3));
    add("i16", KvValue::Int16(-4));
    add("u16", KvValue::UInt16(5));
    add("z64", KvValue::Int64(0));
    add("o64", KvValue::Int64(1));
    add("i64", KvValue::Int64(99));
    add("u64", KvValue::UInt64(6));
    add("z64f", KvValue::Double(0.0));
    add("o64f", KvValue::Double(1.0));
    add("f64", KvValue::Double(2.5));
    add("f32", KvValue::Float(1.5f));
    add("empty", KvValue::String(""));
    add("text", KvValue::String("hero"));
    add("again", KvValue::String("hero"));
    auto resource = KvValue::String("models/heroes/test/test.vmdl");
    resource.Flag = KvFlag::Resource;
    add("res", std::move(resource));
    auto resource_name = KvValue::String("test");
    resource_name.Flag = KvFlag::ResourceName;
    add("resname", std::move(resource_name));
    add("blob", KvValue::Blob(std::vector<uint8_t>{9, 8, 7}));
    add("emptyBlob", KvValue::Blob({}));
    add("emptyArray", KvValue::Array({}));
    add("nums", KvValue::Array({KvValue::Int32(1), KvValue::Int32(2)}));
    add("child", KvValue::Object({{"n", KvValue::Int32(7)}}));
    add("", KvValue::String("blank-key"));
    return KvValue::Object(std::move(fields));
}

}  // namespace

TEST(Kv3, RecognizesBinaryMagic) {
    EXPECT_TRUE(skeleton_repair::IsBinaryKv3(0x03564B56u));
    EXPECT_TRUE(skeleton_repair::IsBinaryKv3(0x4B563301u));
    EXPECT_TRUE(skeleton_repair::IsBinaryKv3(0x4B563305u));
    EXPECT_FALSE(skeleton_repair::IsBinaryKv3(0x4B563300u));
    EXPECT_FALSE(skeleton_repair::IsBinaryKv3(0x4B563306u));
}

TEST(Kv3, EmptyObjectMatchesVrf) {
    const auto expected = Load("empty-object.kv3");
    ASSERT_EQ(expected.size(), 133u);
    EXPECT_EQ(expected[4], skeleton_repair::KvDocument::GenericFormat[0]);
    EXPECT_EQ(expected[19], skeleton_repair::KvDocument::GenericFormat[15]);

    skeleton_repair::KvDocument document;
    document.Root = skeleton_repair::KvValue::Object({});
    ExpectBytes(skeleton_repair::WriteKv3(document), expected);

    const auto read = skeleton_repair::ReadKv3(expected);
    EXPECT_EQ(read.Version, 5);
    EXPECT_EQ(read.Format, skeleton_repair::KvDocument::GenericFormat);
    EXPECT_TRUE(read.Root.IsObject());
    EXPECT_EQ(read.Root.Count(), 0u);
    ExpectBytes(skeleton_repair::WriteKv3(read), expected);
}

TEST(Kv3, ScalarsMatchVrfVersions4And5) {
    skeleton_repair::KvDocument document;
    document.Root = Scalars();
    ExpectBytes(skeleton_repair::WriteKv3(document), Load("scalars.kv3"));

    document.Version = 4;
    ExpectBytes(skeleton_repair::WriteKv3(document), Load("scalars-v4.kv3"));

    const auto read = skeleton_repair::ReadKv3(Load("scalars.kv3"));
    EXPECT_EQ(read.Root.Find("i32")->Int32(), -2);
    EXPECT_EQ(read.Root.Find("u32")->UInt32(), 3u);
    EXPECT_EQ(read.Root.Find("i16")->Int16(), -4);
    EXPECT_EQ(read.Root.Find("z64")->Int64(), 0);
    EXPECT_EQ(read.Root.Find("z64")->Kind, skeleton_repair::KvKind::Int64);
    EXPECT_DOUBLE_EQ(read.Root.Find("f64")->Double(), 2.5);
    EXPECT_FLOAT_EQ(read.Root.Find("f32")->Float(), 1.5f);
    EXPECT_EQ(read.Root.Find("empty")->String(), "");
    EXPECT_EQ(read.Root.Find("res")->Flag, skeleton_repair::KvFlag::Resource);
    EXPECT_EQ(read.Root.Find("resname")->Flag, skeleton_repair::KvFlag::ResourceName);
    EXPECT_EQ(read.Root.Find("blob")->Blob(), (std::vector<uint8_t>{9, 8, 7}));
    EXPECT_TRUE(read.Root.Find("emptyBlob")->Blob().empty());
    EXPECT_EQ(read.Root.Find("emptyArray")->Count(), 0u);
    EXPECT_EQ(read.Root.Find("nums")->At(1).Int32(), 2);
    EXPECT_EQ(read.Root.Find("child")->Find("n")->Int32(), 7);
    EXPECT_EQ(read.Root.Find("")->String(), "blank-key");
    ExpectBytes(skeleton_repair::WriteKv3(skeleton_repair::ReadKv3(Load("scalars-v4.kv3"))), Load("scalars-v4.kv3"));
}

TEST(Kv3, ModelAndAnimationRoundTrip) {
    for (const char* name : {"model.kv3", "anim.kv3"}) {
        const auto bytes = Load(name);
        const auto read = skeleton_repair::ReadKv3(bytes);
        ExpectBytes(skeleton_repair::WriteKv3(read), bytes);
    }

    const auto model = skeleton_repair::ReadKv3(Load("model.kv3"));
    EXPECT_EQ(model.Root.Find("m_name")->String(), "models/heroes/test/test.vmdl");
    const auto* skeleton = model.Root.Find("m_modelSkeleton");
    ASSERT_NE(skeleton, nullptr);
    EXPECT_EQ(skeleton->Find("m_boneName")->At(1).String(), "arm");
    EXPECT_EQ(skeleton->Find("m_nParent")->At(0).Int32(), -1);
    EXPECT_FLOAT_EQ(skeleton->Find("m_boneScaleParent")->At(0).Float(), 1.f);
    EXPECT_EQ(model.Root.Find("m_remappingTable")->At(1).ToInt32(), 1);

    const auto anim = skeleton_repair::ReadKv3(Load("anim.kv3"));
    EXPECT_FALSE(anim.Root.Find("m_bIsPropSkeleton")->Bool());
    EXPECT_EQ(anim.Root.Find("m_secondarySkeletons")->Count(), 0u);
    EXPECT_EQ(anim.Root.Find("m_numBonesToSampleAtLowLOD")->Int32(), 2);
    EXPECT_FLOAT_EQ(anim.Root.Find("m_maskDefinitions")->At(0).Find("m_authoredValue")->Float(), 0.75f);
    EXPECT_FLOAT_EQ(anim.Root.Find("m_parentSpaceReferencePose")->At(0).At(0).Float(), 7.f);
}

TEST(Kv3, ReadsLz4AndZstdAsUncompressedDocuments) {
    const auto model = Load("model.kv3");
    ExpectBytes(skeleton_repair::WriteKv3(skeleton_repair::ReadKv3(Load("model-lz4.kv3"))), model);
    ExpectBytes(skeleton_repair::WriteKv3(skeleton_repair::ReadKv3(Load("model-zstd.kv3"))), model);

    const auto blob = Load("blob.kv3");
    const auto lz4 = skeleton_repair::ReadKv3(Load("blob-lz4.kv3"));
    const auto zstd = skeleton_repair::ReadKv3(Load("blob-zstd.kv3"));
    EXPECT_EQ(lz4.Root.Find("data")->Blob().size(), 20000u);
    EXPECT_EQ(lz4.Root.Find("tail")->Int32(), 3);
    ExpectBytes(skeleton_repair::WriteKv3(lz4), blob);
    ExpectBytes(skeleton_repair::WriteKv3(zstd), blob);
}

TEST(Kv3, RejectsATruncatedOrForeignSignature) {
    EXPECT_THROW(skeleton_repair::ReadKv3({}), skeleton_repair::InvalidData);
    const std::vector<uint8_t> foreign{0x00, 0x33, 0x56, 0x4B};
    EXPECT_THROW(skeleton_repair::ReadKv3(foreign), skeleton_repair::InvalidData);
    skeleton_repair::KvDocument document;
    document.Version = 3;
    EXPECT_THROW(skeleton_repair::WriteKv3(document), skeleton_repair::InvalidOperation);
}
