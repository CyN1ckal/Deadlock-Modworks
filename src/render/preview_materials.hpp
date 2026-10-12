#pragma once

#include "preview_scene.hpp"
#include "texture.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// What the preview needs from one material: its color map and how it blends.
struct PreviewMaterial {
    // Material resource name as the draw calls use it (".vmat").
    std::string Name;
    std::optional<TextureImage> Color;
    bool AlphaTest = false;
    bool Translucent = false;
    // Why there is no color map, when there is none.
    std::string Problem;
};

// Returns a compiled resource's bytes by path ("..._c"), or nothing when it is missing.
using ResourceReader = std::function<std::optional<std::vector<uint8_t>>(const std::string& path)>;

// One entry per distinct material in the scene, in first-use order. Missing or unreadable
// materials and textures are reported in Problem, never thrown.
std::vector<PreviewMaterial> LoadPreviewMaterials(const PreviewScene& scene, const ResourceReader& read, uint32_t max_texture_size = 2048);

}  // namespace skeleton_repair
