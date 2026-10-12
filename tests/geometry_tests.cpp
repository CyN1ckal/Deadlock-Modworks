#include "error.hpp"
#include "kv3.hpp"
#include "model_geometry.hpp"

#include <gtest/gtest.h>
#include <meshoptimizer.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using skeleton_repair::DecodeModelGeometry;
using skeleton_repair::InvalidData;
using skeleton_repair::KvDocument;
using skeleton_repair::KvValue;
using skeleton_repair::ModelGeometry;
using skeleton_repair::WriteKv3;

using Fields = std::vector<std::pair<std::string, KvValue>>;

template <typename T>
void Put(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<uint8_t> Kv(KvValue root) {
    KvDocument document;
    document.Root = std::move(root);
    return WriteKv3(document);
}

KvValue Floats(std::initializer_list<float> values) {
    std::vector<KvValue> items;
    for (float value : values) {
        items.push_back(KvValue::Float(value));
    }
    return KvValue::Array(std::move(items));
}

KvValue Ints(std::initializer_list<int32_t> values) {
    std::vector<KvValue> items;
    for (int32_t value : values) {
        items.push_back(KvValue::Int32(value));
    }
    return KvValue::Array(std::move(items));
}

// Version 12 resource with the blocks laid out in order after the table.
std::vector<uint8_t> Resource(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& blocks) {
    const size_t table = 16;
    size_t at = table + blocks.size() * 12;
    std::vector<uint8_t> file(at);
    for (const auto& [type, data] : blocks) {
        file.insert(file.end(), data.begin(), data.end());
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

KvValue Field(const char* semantic, int format, int offset) {
    return KvValue::Object(Fields{
        {"m_pSemanticName", KvValue::String(semantic)},
        {"m_nSemanticIndex", KvValue::Int32(0)},
        {"m_Format", KvValue::Int32(format)},
        {"m_nOffset", KvValue::Int32(offset)},
        {"m_nSlot", KvValue::Int32(0)},
        {"m_nSlotType", KvValue::String("RENDER_SLOT_PER_VERTEX")},
        {"m_szShaderSemantic", KvValue::String("")},
    });
}

KvValue BufferDescription(int block, int count, int stride, bool meshopt, bool zstd, std::vector<KvValue> fields) {
    return KvValue::Object(Fields{
        {"m_nBlockIndex", KvValue::Int32(block)},
        {"m_nElementCount", KvValue::Int32(count)},
        {"m_nElementSizeInBytes", KvValue::Int32(stride)},
        {"m_bMeshoptCompressed", KvValue::Bool(meshopt)},
        {"m_bMeshoptIndexSequence", KvValue::Bool(false)},
        {"m_bCompressedZSTD", KvValue::Bool(zstd)},
        {"m_inputLayoutFields", KvValue::Array(std::move(fields))},
    });
}

KvValue Binding(int handle) {
    return KvValue::Object(Fields{{"m_hBuffer", KvValue::Int32(handle)}, {"m_nBindOffsetBytes", KvValue::Int32(0)}});
}

KvValue MeshData(int start, int count, int base) {
    KvValue call = KvValue::Object(Fields{
        {"m_nPrimitiveType", KvValue::String("RENDER_PRIM_TRIANGLES")},
        {"m_nBaseVertex", KvValue::Int32(base)},
        {"m_nStartIndex", KvValue::Int32(start)},
        {"m_nIndexCount", KvValue::Int32(count)},
        {"m_indexBuffer", Binding(0)},
        {"m_material", KvValue::String("models/test/materials/body.vmat")},
        {"m_vertexBuffers", KvValue::Array({Binding(0)})},
    });
    KvValue scene = KvValue::Object(Fields{
        {"m_vMinBounds", Floats({-1, -2, -3})},
        {"m_vMaxBounds", Floats({4, 5, 6})},
        {"m_drawCalls", KvValue::Array({std::move(call)})},
    });
    return KvValue::Object(Fields{{"_class", KvValue::String("CRenderMesh")}, {"m_sceneObjects", KvValue::Array({std::move(scene)})}});
}

// Three bones: root at (0,0,1), child turned 90 degrees about Z, grandchild one unit along the child's X.
KvValue ModelData(KvValue remap_table) {
    const float half = 0.70710678f;
    KvValue skeleton = KvValue::Object(Fields{
        {"m_boneName", KvValue::Array({KvValue::String("root"), KvValue::String("arm"), KvValue::String("hand")})},
        {"m_nParent", Ints({-1, 0, 1})},
        {"m_bonePosParent", KvValue::Array({Floats({0, 0, 1}), Floats({1, 0, 0}), Floats({1, 0, 0})})},
        {"m_boneRotParent", KvValue::Array({Floats({0, 0, 0, 1}), Floats({0, 0, half, half}), Floats({0, 0, 0, 1})})},
    });
    return KvValue::Object(Fields{
        {"m_name", KvValue::String("models/test/test.vmdl")},
        {"m_refMeshes", KvValue::Array({})},
        {"m_refMeshGroupMasks", KvValue::Array({KvValue::UInt64(1)})},
        {"m_refLODGroupMasks", KvValue::Array({KvValue::UInt64(3)})},
        {"m_nDefaultMeshGroupMask", KvValue::UInt64(1)},
        {"m_modelSkeleton", std::move(skeleton)},
        {"m_remappingTable", std::move(remap_table)},
        {"m_remappingTableStarts", Ints({0})},
    });
}

KvValue Control(KvValue vertex, KvValue index) {
    KvValue mesh = KvValue::Object(Fields{
        {"m_Name", KvValue::String("body")},
        {"m_nMeshIndex", KvValue::Int32(0)},
        {"m_nDataBlock", KvValue::Int32(2)},
        {"m_nMorphBlock", KvValue::Int32(-1)},
        {"m_vertexBuffers", KvValue::Array({std::move(vertex)})},
        {"m_indexBuffers", KvValue::Array({std::move(index)})},
    });
    return KvValue::Object(Fields{{"embedded_meshes", KvValue::Array({std::move(mesh)})}});
}

constexpr int kStride = 28;

// Deadlock hero layout: float3 position, R16G16_SNORM uv, R32_UINT packed normal, RGBA8 bones and weights.
std::vector<uint8_t> HeroVertices() {
    std::vector<uint8_t> bytes(4 * kStride);
    const float positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const int16_t uvs[4][2] = {{0, 0}, {32767, 0}, {32767, -32767}, {0, 16384}};
    // x = 511, y = 511 is the octahedral centre, which decodes to roughly +Z.
    const uint32_t normal = (511u << 12) | (511u << 22);
    const uint8_t bones[4][4] = {{1, 0, 0, 0}, {2, 0, 0, 0}, {0, 2, 0, 0}, {1, 0, 0, 0}};
    const uint8_t weights[4][4] = {{255, 0, 0, 0}, {128, 127, 0, 0}, {255, 0, 0, 0}, {255, 0, 0, 0}};
    for (size_t v = 0; v < 4; ++v) {
        uint8_t* at = bytes.data() + v * kStride;
        std::memcpy(at, positions[v], 12);
        std::memcpy(at + 12, uvs[v], 4);
        std::memcpy(at + 16, &normal, 4);
        std::memcpy(at + 20, bones[v], 4);
        std::memcpy(at + 24, weights[v], 4);
    }
    return bytes;
}

std::vector<KvValue> HeroLayout() {
    return {Field("position", 6, 0), Field("texcoord", 37, 12), Field("normal", 42, 16), Field("blendindices", 30, 20),
        Field("blendweights", 28, 24)};
}

const std::vector<uint16_t> kIndices = {0, 1, 2, 0, 2, 3};

std::vector<uint8_t> EncodeVertices(const std::vector<uint8_t>& raw, size_t count, size_t stride) {
    meshopt_encodeVertexVersion(1);
    std::vector<uint8_t> out(meshopt_encodeVertexBufferBound(count, stride));
    out.resize(meshopt_encodeVertexBuffer(out.data(), out.size(), raw.data(), count, stride));
    return out;
}

std::vector<uint8_t> EncodeIndices(const std::vector<uint16_t>& indices, size_t vertex_count) {
    std::vector<uint8_t> out(meshopt_encodeIndexBufferBound(indices.size(), vertex_count));
    out.resize(meshopt_encodeIndexBuffer(out.data(), out.size(), indices.data(), indices.size()));
    return out;
}

std::vector<uint8_t> Zstd(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    out.resize(ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3));
    return out;
}

std::vector<uint8_t> HeroModel(bool zstd = false, int start = 0, int count = 6, KvValue remap = Ints({2, 0, 1})) {
    std::vector<uint8_t> vertices = EncodeVertices(HeroVertices(), 4, kStride);
    std::vector<uint8_t> indices = EncodeIndices(kIndices, 4);
    if (zstd) {
        vertices = Zstd(vertices);
        indices = Zstd(indices);
    }
    return Resource({
        {"MVTX", vertices},
        {"MIDX", indices},
        {"MDAT", Kv(MeshData(start, count, 0))},
        {"DATA", Kv(ModelData(std::move(remap)))},
        {"CTRL", Kv(Control(BufferDescription(0, 4, kStride, true, zstd, HeroLayout()), BufferDescription(1, 6, 2, true, zstd, {})))},
    });
}

void ExpectNear(const skeleton_repair::Vec3& actual, float x, float y, float z, float tolerance = 1e-5f) {
    EXPECT_NEAR(actual.X, x, tolerance);
    EXPECT_NEAR(actual.Y, y, tolerance);
    EXPECT_NEAR(actual.Z, z, tolerance);
}

void ExpectHeroMesh(const ModelGeometry& geometry) {
    ASSERT_EQ(geometry.Meshes.size(), 1u);
    const auto& mesh = geometry.Meshes[0];
    EXPECT_EQ(mesh.Name, "body");
    EXPECT_EQ(mesh.LodMask, 3u);
    EXPECT_EQ(mesh.MeshGroupMask, 1u);
    EXPECT_TRUE(mesh.InLod(1));
    EXPECT_FALSE(mesh.InLod(2));
    ExpectNear(mesh.MinBounds, -1, -2, -3);
    ExpectNear(mesh.MaxBounds, 4, 5, 6);

    ASSERT_EQ(mesh.VertexBuffers.size(), 1u);
    const auto& vb = mesh.VertexBuffers[0];
    ASSERT_EQ(vb.VertexCount, 4u);
    ExpectNear(vb.Positions[2], 1, 1, 0);
    ExpectNear(vb.Normals[0], 0, 0, 1, 0.01f);
    EXPECT_FLOAT_EQ(vb.TexCoords[1].X, 1.0f);
    EXPECT_FLOAT_EQ(vb.TexCoords[2].Y, -1.0f);
    EXPECT_NEAR(vb.TexCoords[3].Y, 0.5f, 1e-4f);

    // Remap {2, 0, 1}: local 1 -> bone 0, local 2 -> bone 1, local 0 -> bone 2.
    EXPECT_EQ(vb.BoneIndices[0][0], 0);
    EXPECT_FLOAT_EQ(vb.BoneWeights[0][0], 1.0f);
    EXPECT_EQ(vb.BoneIndices[1][0], 1);
    EXPECT_EQ(vb.BoneIndices[1][1], 2);
    EXPECT_NEAR(vb.BoneWeights[1][0], 128.0f / 255.0f, 1e-5f);
    EXPECT_NEAR(vb.BoneWeights[1][1], 127.0f / 255.0f, 1e-5f);
    EXPECT_FLOAT_EQ(vb.BoneWeights[1][2], 0.0f);

    ASSERT_EQ(mesh.IndexBuffers.size(), 1u);
    EXPECT_EQ(mesh.IndexBuffers[0], (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    ASSERT_EQ(mesh.DrawCalls.size(), 1u);
    EXPECT_EQ(mesh.DrawCalls[0].IndexCount, 6u);
    EXPECT_EQ(mesh.DrawCalls[0].Material, "models/test/materials/body.vmat");
}

TEST(ModelGeometry, DecodesMeshoptHeroMesh) {
    const ModelGeometry geometry = DecodeModelGeometry(HeroModel());
    ExpectHeroMesh(geometry);
    EXPECT_EQ(geometry.DefaultMeshGroupMask, 1u);
    EXPECT_TRUE(geometry.ExternalMeshes.empty());
}

TEST(ModelGeometry, DecodesZstdWrappedMeshopt) {
    ExpectHeroMesh(DecodeModelGeometry(HeroModel(true)));
}

TEST(ModelGeometry, ComposesBindPoseInModelSpace) {
    const ModelGeometry geometry = DecodeModelGeometry(HeroModel());
    ASSERT_EQ(geometry.Bones.size(), 3u);
    EXPECT_EQ(geometry.Bones[2].Name, "hand");
    EXPECT_EQ(geometry.Bones[2].Parent, 1);
    ExpectNear(geometry.Bones[0].ModelPosition, 0, 0, 1);
    ExpectNear(geometry.Bones[1].ModelPosition, 1, 0, 1);
    ExpectNear(geometry.Bones[2].ModelPosition, 1, 1, 1);
    EXPECT_NEAR(geometry.Bones[2].ModelRotation.Z, 0.70710678f, 1e-5f);
    EXPECT_NEAR(geometry.Bones[2].ModelRotation.W, 0.70710678f, 1e-5f);
}

TEST(ModelGeometry, DecodesUncompressedRigidMesh) {
    // Weapon-style buffer: float position and normal, blend indices without weights, inline 32-bit indices.
    constexpr int stride = 28;
    std::vector<uint8_t> vertices(3 * stride);
    for (int v = 0; v < 3; ++v) {
        const float position[3] = {static_cast<float>(v), 2, 3};
        const float normal[3] = {0, 1, 0};
        const uint8_t bones[4] = {static_cast<uint8_t>(v == 2 ? 1 : 0), 0, 0, 0};
        std::memcpy(vertices.data() + v * stride, position, 12);
        std::memcpy(vertices.data() + v * stride + 12, normal, 12);
        std::memcpy(vertices.data() + v * stride + 24, bones, 4);
    }
    std::vector<uint8_t> indices(12);
    const uint32_t triangle[3] = {2, 1, 0};
    std::memcpy(indices.data(), triangle, 12);

    KvValue index = KvValue::Object(Fields{
        {"m_nElementCount", KvValue::Int32(3)},
        {"m_nElementSizeInBytes", KvValue::Int32(4)},
        {"m_pData", KvValue::Blob(indices)},
    });
    const auto file = Resource({
        {"MVTX", vertices},
        {"DATA", Kv(ModelData(Ints({1, 2})))},
        {"MDAT", Kv(MeshData(0, 3, 0))},
        {"CTRL", Kv(Control(BufferDescription(0, 3, stride, false, false,
                                {Field("POSITION", 6, 0), Field("NORMAL", 6, 12), Field("BLENDINDICES", 30, 24)}),
                     std::move(index)))},
    });
    // The CTRL above names data block 2, which is MDAT in this layout.
    const ModelGeometry geometry = DecodeModelGeometry(file);
    ASSERT_EQ(geometry.Meshes.size(), 1u);
    const auto& vb = geometry.Meshes[0].VertexBuffers[0];
    ExpectNear(vb.Positions[2], 2, 2, 3);
    ExpectNear(vb.Normals[1], 0, 1, 0);
    EXPECT_TRUE(vb.TexCoords.empty());
    EXPECT_EQ(vb.BoneIndices[0][0], 1);
    EXPECT_EQ(vb.BoneIndices[2][0], 2);
    EXPECT_FLOAT_EQ(vb.BoneWeights[2][0], 1.0f);
    EXPECT_EQ(geometry.Meshes[0].IndexBuffers[0], (std::vector<uint32_t>{2, 1, 0}));
}

TEST(ModelGeometry, RejectsDrawCallOutsideIndexBuffer) {
    EXPECT_THROW(DecodeModelGeometry(HeroModel(false, 3, 6)), InvalidData);
}

TEST(ModelGeometry, RejectsBlendIndexOutsideRemapTable) {
    EXPECT_THROW(DecodeModelGeometry(HeroModel(false, 0, 6, Ints({2, 0}))), InvalidData);
}

TEST(ModelGeometry, RejectsCorruptMeshoptData) {
    auto file = HeroModel();
    // Block 0 (MVTX) starts right after the five-entry table; the first byte is the codec header.
    file[16 + 5 * 12] = 0xFF;
    EXPECT_THROW(DecodeModelGeometry(file), InvalidData);
}

TEST(ModelGeometry, ModelWithoutEmbeddedMeshesHasOnlyBones) {
    KvValue data = ModelData(Ints({}));
    data.Set("m_refMeshes", KvValue::Array({KvValue::String("models/test/test_body.vmesh")}));
    const ModelGeometry geometry = DecodeModelGeometry(Resource({{"DATA", Kv(std::move(data))}}));
    EXPECT_TRUE(geometry.Meshes.empty());
    EXPECT_EQ(geometry.Bones.size(), 3u);
    EXPECT_EQ(geometry.ExternalMeshes, (std::vector<std::string>{"models/test/test_body.vmesh"}));
}

}  // namespace
