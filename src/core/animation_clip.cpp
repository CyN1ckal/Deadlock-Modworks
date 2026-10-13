#include "animation_clip.hpp"

#include <algorithm>
#include <cmath>

namespace skeleton_repair {
namespace {

float DecodeFloat(uint16_t value, const AnimationClip::Range& range) {
    return static_cast<float>(value) / 65535.0f * range.Length + range.Start;
}

// Smallest-three quaternion: 15 bits per stored component, the dropped component's index in the two top bits.
Quat DecodeQuaternion(const uint16_t* data) {
    constexpr float kMin = -0.70710678f;
    constexpr float kLength = 1.41421356f;
    const float a = static_cast<float>(data[0] & 0x7FFF) * (kLength / 32767.0f) + kMin;
    const float b = static_cast<float>(data[1] & 0x7FFF) * (kLength / 32767.0f) + kMin;
    const float c = static_cast<float>(data[2]) * (kLength / 32767.0f) + kMin;
    const float d = std::sqrt(std::max(0.0f, 1.0f - (a * a + b * b + c * c)));
    switch (((data[0] >> 14) & 2) | (data[1] >> 15)) {
    case 0: return Quat{d, a, b, c};
    case 1: return Quat{a, d, b, c};
    case 2: return Quat{a, b, d, c};
    default: return Quat{a, b, c, d};
    }
}

Quat Nlerp(Quat a, Quat b, float t) {
    // Take the short way round.
    if (a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W < 0) {
        b = Quat{-b.X, -b.Y, -b.Z, -b.W};
    }
    Quat q{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t, a.W + (b.W - a.W) * t};
    const float length = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    return length > 0 ? Quat{q.X / length, q.Y / length, q.Z / length, q.W / length} : Quat{};
}

Quat Multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z,
    };
}

}  // namespace

void AnimationClip::SampleFrame(uint32_t frame, std::span<BoneTransform> out) const {
    if (FrameCount == 0) {
        return;
    }
    frame = std::min(frame, FrameCount - 1);
    const uint16_t* data = data_.data() + frame_offsets_[frame];
    const size_t count = std::min(out.size(), Tracks.size());
    for (size_t i = 0; i < count; ++i) {
        const Track& track = Tracks[i];
        BoneTransform& bone = out[i];
        bone.Rotation = track.ConstantRotation;
        bone.Translation = Vec3{track.TranslationX.Start, track.TranslationY.Start, track.TranslationZ.Start};
        bone.Scale = track.Scale.Start;
        if (!track.StaticRotation) {
            bone.Rotation = DecodeQuaternion(data);
            data += 3;
        }
        if (!track.StaticTranslation) {
            bone.Translation = Vec3{DecodeFloat(data[0], track.TranslationX), DecodeFloat(data[1], track.TranslationY), DecodeFloat(data[2], track.TranslationZ)};
            data += 3;
        }
        if (!track.StaticScale) {
            bone.Scale = DecodeFloat(data[0], track.Scale);
            data += 1;
        }
    }
}

void AnimationClip::Sample(float seconds, std::span<BoneTransform> out) const {
    if (FrameCount <= 1 || Duration <= 0) {
        SampleFrame(0, out);
        return;
    }
    const float wrapped = std::fmod(std::fmod(seconds, Duration) + Duration, Duration);
    const float position = wrapped / Duration * static_cast<float>(FrameCount - 1);
    const uint32_t first = std::min(static_cast<uint32_t>(position), FrameCount - 2);
    const float t = position - static_cast<float>(first);

    SampleFrame(first, out);
    std::vector<BoneTransform> next(out.size());
    SampleFrame(first + 1, next);
    const size_t count = std::min(out.size(), Tracks.size());
    for (size_t i = 0; i < count; ++i) {
        BoneTransform& a = out[i];
        const BoneTransform& b = next[i];
        a.Rotation = Nlerp(a.Rotation, b.Rotation, t);
        a.Translation = Vec3{a.Translation.X + (b.Translation.X - a.Translation.X) * t, a.Translation.Y + (b.Translation.Y - a.Translation.Y) * t,
            a.Translation.Z + (b.Translation.Z - a.Translation.Z) * t};
        a.Scale += (b.Scale - a.Scale) * t;
    }
}

BoneTransform ApplyAdditive(const BoneTransform& reference, const BoneTransform& delta) {
    BoneTransform result;
    result.Rotation = Multiply(delta.Rotation, reference.Rotation);
    result.Translation = Vec3{reference.Translation.X + delta.Translation.X, reference.Translation.Y + delta.Translation.Y,
        reference.Translation.Z + delta.Translation.Z};
    result.Scale = reference.Scale;
    return result;
}

}  // namespace skeleton_repair
