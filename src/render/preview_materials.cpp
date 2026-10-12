#include "preview_materials.hpp"

#include <exception>
#include <unordered_set>

namespace skeleton_repair {

std::vector<PreviewMaterial> LoadPreviewMaterials(const PreviewScene& scene, const ResourceReader& read, uint32_t max_texture_size) {
    std::vector<PreviewMaterial> materials;
    std::unordered_set<std::string> seen;
    for (const PreviewDraw& draw : scene.Draws) {
        if (draw.Material.empty() || !seen.insert(draw.Material).second) {
            continue;
        }
        PreviewMaterial material;
        material.Name = draw.Material;
        try {
            const std::optional<std::vector<uint8_t>> material_bytes = read(draw.Material + "_c");
            if (!material_bytes) {
                material.Problem = "Material is not in the mod or the game.";
            } else {
                const MaterialInfo info = DecodeMaterial(*material_bytes);
                material.AlphaTest = info.IntParam("F_ALPHA_TEST") != 0;
                material.Translucent = info.IntParam("F_TRANSLUCENT") != 0;
                const std::optional<std::string> color = info.ColorTexture();
                if (!color) {
                    material.Problem = "Material has no color texture.";
                } else if (const std::optional<std::vector<uint8_t>> texture_bytes = read(*color + "_c")) {
                    material.Color = DecodeTexture(*texture_bytes, max_texture_size);
                } else {
                    material.Problem = "Color texture " + *color + " is missing.";
                }
            }
        } catch (const std::exception& error) {
            material.Color.reset();
            material.Problem = error.what();
        }
        materials.push_back(std::move(material));
    }
    return materials;
}

}  // namespace skeleton_repair
