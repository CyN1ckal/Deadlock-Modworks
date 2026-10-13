#include "clip_decoder.hpp"

#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"

#include <cstring>
#include <string>

namespace skeleton_repair {
namespace {

const KvValue& Require(const KvValue& object, const char* key) {
    const KvValue* value = object.IsObject() ? object.Find(key) : nullptr;
    if (value == nullptr) {
        throw InvalidData(std::string("Animation clip is missing ") + key + ".");
    }
    return *value;
}

bool Flag(const KvValue& object, const char* key) {
    const KvValue* value = object.IsObject() ? object.Find(key) : nullptr;
    if (value == nullptr) {
        return false;
    }
    return value->Kind == KvKind::Bool ? value->Bool() : value->ToInt32() != 0;
}

AnimationClip::Range ReadRange(const KvValue& track, const char* key) {
    const KvValue& range = Require(track, key);
    return {static_cast<float>(Require(range, "m_flRangeStart").ToDouble()), static_cast<float>(Require(range, "m_flRangeLength").ToDouble())};
}

}  // namespace

AnimationClip DecodeAnimationClip(std::span<const uint8_t> file) {
    const std::span<const uint8_t> data = FindDataBlock(file);
    uint32_t magic = 0;
    if (data.size() >= 4) {
        std::memcpy(&magic, data.data(), 4);
    }
    if (!IsBinaryKv3(magic)) {
        throw InvalidData("Animation clip DATA is not KV3.");
    }
    const KvValue root = ReadKv3(data).Root;

    AnimationClip clip;
    clip.Skeleton = Require(root, "m_skeleton").String();
    const int frames = Require(root, "m_nNumFrames").ToInt32();
    if (frames < 0) {
        throw InvalidData("Animation clip has a negative frame count.");
    }
    clip.FrameCount = static_cast<uint32_t>(frames);
    clip.Duration = static_cast<float>(Require(root, "m_flDuration").ToDouble());
    clip.Additive = Flag(root, "m_bIsAdditive");

    const KvValue& tracks = Require(root, "m_trackCompressionSettings");
    if (!tracks.IsArray()) {
        throw InvalidData("m_trackCompressionSettings is not an array.");
    }
    for (const KvValue& item : tracks.Items()) {
        AnimationClip::Track track;
        track.TranslationX = ReadRange(item, "m_translationRangeX");
        track.TranslationY = ReadRange(item, "m_translationRangeY");
        track.TranslationZ = ReadRange(item, "m_translationRangeZ");
        track.Scale = ReadRange(item, "m_scaleRange");
        const KvValue& rotation = Require(item, "m_constantRotation");
        if (!rotation.IsArray() || rotation.Count() != 4) {
            throw InvalidData("Track constant rotation is not a quaternion.");
        }
        track.ConstantRotation = Quat{static_cast<float>(rotation.At(0).ToDouble()), static_cast<float>(rotation.At(1).ToDouble()),
            static_cast<float>(rotation.At(2).ToDouble()), static_cast<float>(rotation.At(3).ToDouble())};
        track.StaticRotation = Flag(item, "m_bIsRotationStatic");
        track.StaticTranslation = Flag(item, "m_bIsTranslationStatic");
        track.StaticScale = Flag(item, "m_bIsScaleStatic");
        clip.frame_size_ += (track.StaticRotation ? 0 : 3) + (track.StaticTranslation ? 0 : 3) + (track.StaticScale ? 0 : 1);
        clip.Tracks.push_back(track);
    }

    const KvValue& pose = Require(root, "m_compressedPoseData");
    if (pose.Kind != KvKind::Blob || pose.Blob().size() % 2 != 0) {
        throw InvalidData("m_compressedPoseData is not a uint16 blob.");
    }
    clip.data_.resize(pose.Blob().size() / 2);
    std::memcpy(clip.data_.data(), pose.Blob().data(), pose.Blob().size());

    const KvValue& offsets = Require(root, "m_compressedPoseOffsets");
    if (!offsets.IsArray() || offsets.Count() < clip.FrameCount) {
        throw InvalidData("Animation clip has fewer pose offsets than frames.");
    }
    for (uint32_t frame = 0; frame < clip.FrameCount; ++frame) {
        const int offset = offsets.At(frame).ToInt32();
        if (offset < 0 || static_cast<size_t>(offset) + clip.frame_size_ > clip.data_.size()) {
            throw InvalidData("Animation frame " + std::to_string(frame) + " is outside the pose data.");
        }
        clip.frame_offsets_.push_back(static_cast<uint32_t>(offset));
    }
    return clip;
}

}  // namespace skeleton_repair
