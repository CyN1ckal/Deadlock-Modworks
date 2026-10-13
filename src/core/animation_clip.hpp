#pragma once

#include "bone_transform.hpp"
#include "skeleton.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

// A compiled AG2 animation clip (.vnmclip_c). Each track animates one bone of the clip's
// animation skeleton, in that skeleton's bone order.
// Layout pinned to ValveResourceFormat 20.0.6980 ModelAnimation2.AnimationClip.
class AnimationClip {
public:
    struct Range {
        float Start = 0;
        float Length = 0;
    };
    struct Track {
        Range TranslationX;
        Range TranslationY;
        Range TranslationZ;
        Range Scale;
        Quat ConstantRotation;
        bool StaticRotation = true;
        bool StaticTranslation = true;
        bool StaticScale = true;
    };

    // The .vnmskel resource the tracks follow.
    std::string Skeleton;
    uint32_t FrameCount = 0;
    float Duration = 0;
    // Additive clips store offsets from the skeleton's reference pose.
    bool Additive = false;
    std::vector<Track> Tracks;

    // Parent-space transforms for one stored frame. `out` should have one entry per track.
    void SampleFrame(uint32_t frame, std::span<BoneTransform> out) const;
    // Blends the two frames around `seconds`, wrapping past the end.
    void Sample(float seconds, std::span<BoneTransform> out) const;

private:
    friend AnimationClip DecodeAnimationClip(std::span<const uint8_t> file);
    std::vector<uint16_t> data_;
    std::vector<uint32_t> frame_offsets_;
    // uint16 values per frame, checked against every frame offset on decode.
    size_t frame_size_ = 0;
};

AnimationClip DecodeAnimationClip(std::span<const uint8_t> file);

// Applies an additive pose onto a reference pose, track by track.
BoneTransform ApplyAdditive(const BoneTransform& reference, const BoneTransform& delta);

}  // namespace skeleton_repair
