#include "clip_decoder.hpp"
#include "error.hpp"
#include "kv3.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using skeleton_repair::AnimationClip;
using skeleton_repair::BoneTransform;
using skeleton_repair::DecodeAnimationClip;
using skeleton_repair::InvalidData;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::Quat;

using Fields = std::vector<std::pair<std::string, KvValue>>;

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

KvValue Range(float start, float length) {
    return KvValue::Object(Fields{{"m_flRangeStart", KvValue::Float(start)}, {"m_flRangeLength", KvValue::Float(length)}});
}

KvValue Track(bool static_rotation, bool static_translation, Quat constant = {}) {
    return KvValue::Object(Fields{
        {"m_translationRangeX", Range(1, 0)},
        {"m_translationRangeY", Range(-10, 20)},
        {"m_translationRangeZ", Range(0, 100)},
        {"m_scaleRange", Range(1, 0)},
        {"m_nTrackReadOffset", KvValue::Int32(0)},
        {"m_constantRotation", KvValue::Array({KvValue::Float(constant.X), KvValue::Float(constant.Y), KvValue::Float(constant.Z), KvValue::Float(constant.W)})},
        {"m_bIsRotationStatic", KvValue::Bool(static_rotation)},
        {"m_bIsTranslationStatic", KvValue::Bool(static_translation)},
        {"m_bIsScaleStatic", KvValue::Bool(true)},
    });
}

// Inverse of the decoder: drop the largest component, store the rest in 15 bits.
std::vector<uint16_t> EncodeQuaternion(Quat q) {
    const float values[4] = {q.X, q.Y, q.Z, q.W};
    int largest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::abs(values[i]) > std::abs(values[largest])) {
            largest = i;
        }
    }
    const float sign = values[largest] < 0 ? -1.0f : 1.0f;
    std::vector<uint16_t> out;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) {
            continue;
        }
        const float normalized = (values[i] * sign + 0.70710678f) / 1.41421356f;
        out.push_back(static_cast<uint16_t>(std::lround(std::clamp(normalized, 0.0f, 1.0f) * 32767.0f)));
    }
    out[0] = static_cast<uint16_t>(out[0] | ((largest & 2) << 14));
    out[1] = static_cast<uint16_t>(out[1] | ((largest & 1) << 15));
    return out;
}

std::vector<uint8_t> Blob(const std::vector<uint16_t>& values) {
    std::vector<uint8_t> bytes(values.size() * 2);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

// Two tracks: a static root, and a bone whose rotation and translation are animated over two frames.
std::vector<uint8_t> TwoFrameClip(Quat first, Quat second, std::vector<int32_t> offsets = {0, 6}, bool additive = false) {
    std::vector<uint16_t> data;
    // Frame 0 sits at the bottom of the Z range, frame 1 at the top.
    for (int frame = 0; frame < 2; ++frame) {
        const auto rotation = EncodeQuaternion(frame == 0 ? first : second);
        data.insert(data.end(), rotation.begin(), rotation.end());
        data.insert(data.end(), {uint16_t{0}, uint16_t{32768}, frame == 0 ? uint16_t{0} : uint16_t{65535}});
    }
    std::vector<KvValue> offset_values;
    for (int32_t offset : offsets) {
        offset_values.push_back(KvValue::Int32(offset));
    }
    return Resource(KvValue::Object(Fields{
        {"m_skeleton", KvValue::String("models/test/test.vnmskel")},
        {"m_nNumFrames", KvValue::Int32(2)},
        {"m_flDuration", KvValue::Float(1)},
        {"m_compressedPoseData", KvValue::Blob(Blob(data))},
        {"m_trackCompressionSettings", KvValue::Array({Track(true, true, Quat{0, 0, 0, 1}), Track(false, false)})},
        {"m_compressedPoseOffsets", KvValue::Array(std::move(offset_values))},
        {"m_bIsAdditive", KvValue::Bool(additive)},
    }));
}

void ExpectQuat(const Quat& actual, const Quat& expected, float tolerance = 1e-4f) {
    // q and -q are the same rotation.
    const float dot = actual.X * expected.X + actual.Y * expected.Y + actual.Z * expected.Z + actual.W * expected.W;
    EXPECT_NEAR(std::abs(dot), 1.0f, tolerance) << actual.X << " " << actual.Y << " " << actual.Z << " " << actual.W;
}

const Quat kQuarterTurnZ{0, 0, 0.70710678f, 0.70710678f};
const Quat kTiltX{0.25881905f, 0, 0, 0.96592583f};

TEST(AnimationClip, ReadsHeaderAndTracks) {
    const AnimationClip clip = DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX));
    EXPECT_EQ(clip.Skeleton, "models/test/test.vnmskel");
    EXPECT_EQ(clip.FrameCount, 2u);
    EXPECT_FLOAT_EQ(clip.Duration, 1.0f);
    EXPECT_FALSE(clip.Additive);
    ASSERT_EQ(clip.Tracks.size(), 2u);
    EXPECT_TRUE(clip.Tracks[0].StaticRotation);
    EXPECT_FALSE(clip.Tracks[1].StaticRotation);
}

TEST(AnimationClip, DecodesStaticAndAnimatedTracks) {
    const AnimationClip clip = DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX));
    std::vector<BoneTransform> bones(2);
    clip.SampleFrame(0, bones);
    ExpectQuat(bones[0].Rotation, Quat{0, 0, 0, 1});
    EXPECT_FLOAT_EQ(bones[0].Translation.X, 1.0f);
    ExpectQuat(bones[1].Rotation, kQuarterTurnZ);
    EXPECT_FLOAT_EQ(bones[1].Translation.X, 1.0f);
    EXPECT_NEAR(bones[1].Translation.Y, 0.0f, 1e-3f);
    EXPECT_NEAR(bones[1].Translation.Z, 0.0f, 1e-4f);

    clip.SampleFrame(1, bones);
    ExpectQuat(bones[1].Rotation, kTiltX);
    EXPECT_NEAR(bones[1].Translation.Z, 100.0f, 1e-3f);
}

TEST(AnimationClip, DecodesEveryDroppedComponent) {
    for (const Quat& q : {Quat{0.9f, 0.3f, 0.2f, 0.2449f}, Quat{0.1f, -0.95f, 0.2f, 0.2179f}, Quat{0.2f, 0.1f, -0.9f, 0.3742f}, kTiltX}) {
        const float length = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
        const Quat unit{q.X / length, q.Y / length, q.Z / length, q.W / length};
        const AnimationClip clip = DecodeAnimationClip(TwoFrameClip(unit, unit));
        std::vector<BoneTransform> bones(2);
        clip.SampleFrame(0, bones);
        ExpectQuat(bones[1].Rotation, unit);
    }
}

TEST(AnimationClip, SampleBlendsFramesAndWraps) {
    const AnimationClip clip = DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kQuarterTurnZ));
    std::vector<BoneTransform> bones(2);
    clip.Sample(0.25f, bones);
    EXPECT_NEAR(bones[1].Translation.Z, 25.0f, 1e-2f);
    // Past the end wraps to the start of the loop.
    clip.Sample(1.25f, bones);
    EXPECT_NEAR(bones[1].Translation.Z, 25.0f, 1e-2f);
    clip.Sample(-0.25f, bones);
    EXPECT_NEAR(bones[1].Translation.Z, 75.0f, 1e-2f);
}

TEST(AnimationClip, FewerOutputsThanTracksIsSafe) {
    const AnimationClip clip = DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX));
    std::vector<BoneTransform> bones(1);
    clip.Sample(0.5f, bones);
    ExpectQuat(bones[0].Rotation, Quat{0, 0, 0, 1});
}

TEST(AnimationClip, RejectsOffsetsOutsideThePoseData) {
    EXPECT_THROW(DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX, {0, 7})), InvalidData);
    EXPECT_THROW(DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX, {0})), InvalidData);
}

TEST(AnimationClip, AdditiveRotatesAndOffsetsTheReference) {
    BoneTransform reference;
    reference.Translation = {1, 2, 3};
    reference.Rotation = kQuarterTurnZ;
    BoneTransform delta;
    delta.Translation = {0, 0, 1};
    delta.Rotation = kQuarterTurnZ;
    const BoneTransform result = skeleton_repair::ApplyAdditive(reference, delta);
    ExpectQuat(result.Rotation, Quat{0, 0, 1, 0});
    EXPECT_FLOAT_EQ(result.Translation.Z, 4.0f);
    EXPECT_TRUE(DecodeAnimationClip(TwoFrameClip(kQuarterTurnZ, kTiltX, {0, 6}, true)).Additive);
}

}  // namespace
