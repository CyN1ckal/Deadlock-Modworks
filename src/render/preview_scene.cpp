#include "preview_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skeleton_repair {
namespace {

constexpr int kMaxLods = 8;

Vec3 Add(Vec3 a, Vec3 b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
Vec3 Scale(Vec3 a, float s) { return {a.X * s, a.Y * s, a.Z * s}; }
float Dot(Vec3 a, Vec3 b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X}; }
Vec3 Normalize(Vec3 a) {
    const float length = std::sqrt(Dot(a, a));
    return length > 0 ? Scale(a, 1 / length) : a;
}

struct Box {
    Vec3 Min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 Max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    bool Empty() const { return Min.X > Max.X; }
    void Add(Vec3 p) {
        Min = {std::min(Min.X, p.X), std::min(Min.Y, p.Y), std::min(Min.Z, p.Z)};
        Max = {std::max(Max.X, p.X), std::max(Max.Y, p.Y), std::max(Max.Z, p.Z)};
    }
};

constexpr uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b) { return PackRgba(r, g, b); }

uint32_t Fnv1a(std::string_view text) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}

std::array<float, 3> HashColor(uint32_t hash, float saturation, float value) {
    const float hue = static_cast<float>(hash % 360) / 60.0f;
    const float chroma = value * saturation;
    const float x = chroma * (1 - std::abs(std::fmod(hue, 2.0f) - 1));
    std::array<float, 3> rgb{};
    switch (static_cast<int>(hue)) {
    case 0: rgb = {chroma, x, 0}; break;
    case 1: rgb = {x, chroma, 0}; break;
    case 2: rgb = {0, chroma, x}; break;
    case 3: rgb = {0, x, chroma}; break;
    case 4: rgb = {x, 0, chroma}; break;
    default: rgb = {chroma, 0, x}; break;
    }
    const float m = value - chroma;
    return {rgb[0] + m, rgb[1] + m, rgb[2] + m};
}

// Muted, stable color per material path so the same material keeps its color between models.
std::array<float, 3> MaterialColor(const std::string& material) {
    return HashColor(Fnv1a(material), 0.45f, 0.8f);
}

// 1, 2 or 5 times a power of ten, close to `target`.
float NiceStep(float target) {
    const float magnitude = std::pow(10.0f, std::floor(std::log10(target)));
    const float fraction = target / magnitude;
    const float nice = fraction < 1.5f ? 1.0f : fraction < 3.5f ? 2.0f : fraction < 7.5f ? 5.0f : 10.0f;
    return nice * magnitude;
}

std::vector<LineVertex> BuildGrid(Vec3 min, Vec3 max) {
    const float extent = std::max({max.X - min.X, max.Y - min.Y, 1.0f}) * 0.75f;
    const float step = NiceStep(extent / 6);
    const int lines = static_cast<int>(std::ceil(extent / step));
    const float half = lines * step;
    const float z = (min.Z <= 0 && max.Z >= 0) ? 0 : min.Z;
    const float cx = std::round((min.X + max.X) * 0.5f / step) * step;
    const float cy = std::round((min.Y + max.Y) * 0.5f / step) * step;

    std::vector<LineVertex> grid;
    for (int i = -lines; i <= lines; ++i) {
        const float x = cx + i * step;
        const float y = cy + i * step;
        const uint32_t along_y = std::abs(x) < step * 0.01f ? Rgba(70, 160, 70) : Rgba(70, 72, 78);
        const uint32_t along_x = std::abs(y) < step * 0.01f ? Rgba(170, 70, 70) : Rgba(70, 72, 78);
        grid.push_back({{x, cy - half, z}, along_y});
        grid.push_back({{x, cy + half, z}, along_y});
        grid.push_back({{cx - half, y, z}, along_x});
        grid.push_back({{cx + half, y, z}, along_x});
    }
    return grid;
}

}  // namespace

uint32_t BoneNameColor(std::string_view name) {
    // Spread neighbouring hashes apart so bones with similar names still differ.
    const std::array<float, 3> rgb = HashColor(Fnv1a(name) * 2654435761u, 0.7f, 0.95f);
    return PackRgba(static_cast<uint8_t>(rgb[0] * 255), static_cast<uint8_t>(rgb[1] * 255), static_cast<uint8_t>(rgb[2] * 255));
}

std::vector<LineVertex> SkeletonLines(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors) {
    std::vector<LineVertex> lines;
    for (size_t i = 0; i < bones.size(); ++i) {
        const GeometryBone& bone = bones[i];
        if (bone.Parent < 0 || static_cast<size_t>(bone.Parent) >= bones.size()) {
            continue;
        }
        const uint32_t color = i < colors.size() ? colors[i] : kBoneLineColor;
        lines.push_back({bones[static_cast<size_t>(bone.Parent)].ModelPosition, color});
        lines.push_back({bone.ModelPosition, color});
    }
    return lines;
}

PreviewScene BuildPreviewScene(const ModelGeometry& geometry) {
    PreviewScene scene;
    scene.DefaultMeshGroupMask = geometry.DefaultMeshGroupMask;
    scene.BoneCount = geometry.Bones.size();
    std::vector<uint32_t> bone_colors;
    bone_colors.reserve(geometry.Bones.size());
    for (const GeometryBone& bone : geometry.Bones) {
        bone_colors.push_back(BoneNameColor(bone.Name));
    }

    for (size_t mesh_index = 0; mesh_index < geometry.Meshes.size(); ++mesh_index) {
        const GeometryMesh& mesh = geometry.Meshes[mesh_index];
        PreviewMesh info;
        info.Name = mesh.Name;
        info.LodMask = mesh.LodMask;
        info.MeshGroupMask = mesh.MeshGroupMask;
        Box box;

        std::vector<int64_t> vertex_base;
        for (const GeometryVertexBuffer& buffer : mesh.VertexBuffers) {
            vertex_base.push_back(static_cast<int64_t>(scene.Vertices.size()));
            for (uint32_t v = 0; v < buffer.VertexCount; ++v) {
                PreviewVertex vertex;
                vertex.Position = v < buffer.Positions.size() ? buffer.Positions[v] : Vec3{};
                vertex.Normal = v < buffer.Normals.size() ? buffer.Normals[v] : Vec3{0, 0, 1};
                vertex.TexCoord = v < buffer.TexCoords.size() ? buffer.TexCoords[v] : Vec2{};
                if (v < buffer.BoneIndices.size()) {
                    const auto& weights = buffer.BoneWeights[v];
                    for (size_t slot = 0; slot < 4; ++slot) {
                        vertex.BoneIndices[slot] = buffer.BoneIndices[v][slot];
                        vertex.BoneWeights[slot] = static_cast<uint8_t>(std::lround(std::clamp(weights[slot], 0.0f, 1.0f) * 255.0f));
                    }
                    const size_t strongest = static_cast<size_t>(std::max_element(weights.begin(), weights.end()) - weights.begin());
                    const size_t bone = buffer.BoneIndices[v][strongest];
                    if (weights[strongest] > 0 && bone < geometry.Bones.size()) {
                        vertex.BoneColor = bone_colors[bone];
                    }
                }
                box.Add(vertex.Position);
                scene.Vertices.push_back(vertex);
            }
            info.VertexCount += buffer.VertexCount;
        }
        std::vector<uint32_t> index_base;
        for (const auto& indices : mesh.IndexBuffers) {
            index_base.push_back(static_cast<uint32_t>(scene.Indices.size()));
            scene.Indices.insert(scene.Indices.end(), indices.begin(), indices.end());
        }
        for (const GeometryDrawCall& call : mesh.DrawCalls) {
            PreviewDraw draw;
            draw.FirstIndex = index_base[call.IndexBuffer] + call.StartIndex;
            draw.IndexCount = call.IndexCount;
            draw.VertexOffset = static_cast<int32_t>(vertex_base[call.VertexBuffer] + call.BaseVertex);
            draw.Mesh = static_cast<uint32_t>(mesh_index);
            draw.MaterialColor = MaterialColor(call.Material);
            draw.Material = call.Material;
            info.TriangleCount += call.IndexCount / 3;
            scene.Draws.push_back(draw);
        }

        info.Min = box.Empty() ? mesh.MinBounds : box.Min;
        info.Max = box.Empty() ? mesh.MaxBounds : box.Max;
        for (int level = kMaxLods - 1; level > 0; --level) {
            if (((mesh.LodMask >> level) & 1) != 0) {
                scene.LodCount = std::max(scene.LodCount, level + 1);
                break;
            }
        }
        scene.Meshes.push_back(std::move(info));
    }

    scene.Skeleton = SkeletonLines(geometry.Bones);

    Box bounds;
    const std::vector<uint8_t> visible = DefaultVisibility(scene, 0);
    for (size_t i = 0; i < scene.Meshes.size(); ++i) {
        if (visible[i] != 0) {
            bounds.Add(scene.Meshes[i].Min);
            bounds.Add(scene.Meshes[i].Max);
        }
    }
    if (bounds.Empty()) {
        for (const GeometryBone& bone : geometry.Bones) {
            bounds.Add(bone.ModelPosition);
        }
    }
    if (bounds.Empty()) {
        bounds.Add({-1, -1, -1});
        bounds.Add({1, 1, 1});
    }
    scene.Min = bounds.Min;
    scene.Max = bounds.Max;
    scene.Grid = BuildGrid(scene.Min, scene.Max);
    return scene;
}

std::vector<uint8_t> DefaultVisibility(const PreviewScene& scene, int lod) {
    std::vector<uint8_t> visible(scene.Meshes.size());
    for (size_t i = 0; i < scene.Meshes.size(); ++i) {
        const PreviewMesh& mesh = scene.Meshes[i];
        const bool in_group = scene.DefaultMeshGroupMask == 0 || mesh.MeshGroupMask == 0
            || (mesh.MeshGroupMask & scene.DefaultMeshGroupMask) != 0;
        visible[i] = mesh.InLod(lod) && in_group ? 1 : 0;
    }
    return visible;
}

OrbitCamera OrbitCamera::Framing(const Vec3& min, const Vec3& max) {
    OrbitCamera camera;
    camera.Target = Scale(Add(min, max), 0.5f);
    const float radius = std::max(std::sqrt(Dot(Sub(max, min), Sub(max, min))) * 0.5f, 0.01f);
    camera.Distance = radius / std::sin(camera.FovY * 0.5f) * 1.05f;
    return camera;
}

Vec3 OrbitCamera::Eye() const {
    const Vec3 direction{std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), std::sin(Pitch)};
    return Add(Target, Scale(direction, Distance));
}

Mat4 OrbitCamera::ViewProjection(float aspect) const {
    const Vec3 eye = Eye();
    const Vec3 f = Normalize(Sub(Target, eye));
    const Vec3 s = Normalize(Cross(f, {0, 0, 1}));
    const Vec3 u = Cross(s, f);
    const Mat4 view = {
        s.X, u.X, -f.X, 0,
        s.Y, u.Y, -f.Y, 0,
        s.Z, u.Z, -f.Z, 0,
        -Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1,
    };

    const float near_plane = Distance * 0.01f;
    const float far_plane = Distance * 100.0f;
    const float focal = 1 / std::tan(FovY * 0.5f);
    Mat4 projection{};
    projection[0] = focal / std::max(aspect, 1e-4f);
    projection[5] = -focal;
    projection[10] = far_plane / (near_plane - far_plane);
    projection[11] = -1;
    projection[14] = near_plane * far_plane / (near_plane - far_plane);

    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += projection[k * 4 + row] * view[column * 4 + k];
            }
            result[column * 4 + row] = sum;
        }
    }
    return result;
}

void OrbitCamera::Orbit(float dx_pixels, float dy_pixels) {
    Yaw -= dx_pixels * 0.008f;
    Pitch = std::clamp(Pitch + dy_pixels * 0.008f, -1.55f, 1.55f);
}

void OrbitCamera::Pan(float dx_pixels, float dy_pixels, float viewport_height) {
    const Vec3 f = Normalize(Sub(Target, Eye()));
    const Vec3 s = Normalize(Cross(f, {0, 0, 1}));
    const Vec3 u = Cross(s, f);
    const float units_per_pixel = 2 * Distance * std::tan(FovY * 0.5f) / std::max(viewport_height, 1.0f);
    Target = Add(Target, Add(Scale(s, -dx_pixels * units_per_pixel), Scale(u, dy_pixels * units_per_pixel)));
}

void OrbitCamera::Zoom(float wheel_steps) {
    Distance = std::max(Distance * std::pow(0.85f, wheel_steps), 0.01f);
}

}  // namespace skeleton_repair
