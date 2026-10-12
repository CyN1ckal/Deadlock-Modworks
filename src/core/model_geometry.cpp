#include "model_geometry.hpp"

#include "error.hpp"
#include "kv3.hpp"

#include <meshoptimizer.h>
#include <zstd.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace skeleton_repair {
namespace {

// DXGI_FORMAT values used by Source 2 input layouts.
enum DxgiFormat : uint32_t {
    R32G32B32A32_SINT = 4,
    R32G32B32_FLOAT = 6,
    R16G16B16A16_UNORM = 11,
    R16G16B16A16_UINT = 12,
    R16G16B16A16_SINT = 14,
    R32G32_FLOAT = 16,
    R8G8B8A8_UNORM = 28,
    R8G8B8A8_UINT = 30,
    R16G16_FLOAT = 34,
    R16G16_UNORM = 35,
    R16G16_SNORM = 37,
    R16G16_SINT = 38,
    R32_UINT = 42,
};

template <typename T>
T Read(std::span<const uint8_t> bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct Block {
    std::array<char, 4> Type{};
    std::span<const uint8_t> Data;
};

std::vector<Block> ReadBlocks(std::span<const uint8_t> file) {
    if (file.size() < 16) {
        throw InvalidData("Resource is too small.");
    }
    if (Read<uint16_t>(file, 4) != 12) {
        throw InvalidData("Resource header is not version 12.");
    }
    const auto block_offset = Read<uint32_t>(file, 8);
    const auto block_count = Read<int32_t>(file, 12);
    if (block_count <= 0 || block_offset < 8) {
        throw InvalidData("Resource block table is invalid.");
    }
    const uint64_t table = 8ULL + block_offset;
    if (table > file.size() || static_cast<uint64_t>(block_count) * 12 > file.size() - table) {
        throw InvalidData("Resource block table is outside the file.");
    }

    std::vector<Block> blocks;
    blocks.reserve(static_cast<size_t>(block_count));
    for (int32_t i = 0; i < block_count; ++i) {
        const size_t pos = static_cast<size_t>(table) + static_cast<size_t>(i) * 12;
        const auto rel = Read<uint32_t>(file, pos + 4);
        const auto size = Read<uint32_t>(file, pos + 8);
        const uint64_t start = static_cast<uint64_t>(pos) + 4 + rel;
        if (start > file.size() || size > file.size() - start) {
            throw InvalidData("Resource block " + std::to_string(i) + " is outside the file.");
        }
        Block block;
        std::memcpy(block.Type.data(), file.data() + pos, 4);
        block.Data = file.subspan(static_cast<size_t>(start), size);
        blocks.push_back(block);
    }
    return blocks;
}

const Block* FindBlock(const std::vector<Block>& blocks, std::string_view type) {
    for (const Block& block : blocks) {
        if (std::string_view(block.Type.data(), 4) == type) {
            return &block;
        }
    }
    return nullptr;
}

const Block& BlockAt(const std::vector<Block>& blocks, int index) {
    if (index < 0 || static_cast<size_t>(index) >= blocks.size()) {
        throw InvalidData("Block index " + std::to_string(index) + " is outside the block table.");
    }
    return blocks[static_cast<size_t>(index)];
}

KvValue ReadKvBlock(const Block& block) {
    if (block.Data.size() < 4 || !IsBinaryKv3(Read<uint32_t>(block.Data, 0))) {
        throw InvalidData(std::string(block.Type.data(), 4) + " block is not KV3.");
    }
    return ReadKv3(block.Data).Root;
}

const KvValue& Require(const KvValue& object, std::string_view key) {
    const KvValue* value = object.IsObject() ? object.Find(key) : nullptr;
    if (value == nullptr) {
        throw InvalidData("Mesh data is missing " + std::string(key) + ".");
    }
    return *value;
}

const KvValue& RequireArray(const KvValue& object, std::string_view key) {
    const KvValue& value = Require(object, key);
    if (!value.IsArray()) {
        throw InvalidData(std::string(key) + " is not an array.");
    }
    return value;
}

uint64_t ToUInt64(const KvValue& value) {
    switch (value.Kind) {
    case KvKind::Int16:
    case KvKind::Int32:
    case KvKind::Int64: {
        const int64_t signed_value = value.Kind == KvKind::Int16 ? value.Int16()
            : value.Kind == KvKind::Int32                       ? value.Int32()
                                                                : value.Int64();
        // Masks are bit sets. A negative value keeps its bits.
        return static_cast<uint64_t>(signed_value);
    }
    case KvKind::UInt16:
        return value.UInt16();
    case KvKind::UInt32:
        return value.UInt32();
    case KvKind::UInt64:
        return value.UInt64();
    default:
        throw InvalidData("KV3 value is not an integer.");
    }
}

int IntOr(const KvValue& object, std::string_view key, int fallback) {
    const KvValue* value = object.Find(key);
    return value == nullptr ? fallback : value->ToInt32();
}

bool FlagOr(const KvValue& object, std::string_view key) {
    const KvValue* value = object.Find(key);
    if (value == nullptr) {
        return false;
    }
    return value->Kind == KvKind::Bool ? value->Bool() : value->ToInt32() != 0;
}

std::string StringOrBlob(const KvValue& value) {
    if (value.Kind == KvKind::String) {
        return value.String();
    }
    if (value.Kind == KvKind::Blob) {
        const auto& blob = value.Blob();
        const auto end = std::find(blob.begin(), blob.end(), uint8_t{0});
        return std::string(blob.begin(), end);
    }
    throw InvalidData("Semantic name is not a string.");
}

std::string Upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

Vec3 ReadVec3(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 3) {
        throw InvalidData("Vector does not have three components.");
    }
    return Vec3{static_cast<float>(value.At(0).ToDouble()), static_cast<float>(value.At(1).ToDouble()),
        static_cast<float>(value.At(2).ToDouble())};
}

Quat ReadQuat(const KvValue& value) {
    if (!value.IsArray() || value.Count() != 4) {
        throw InvalidData("Quaternion does not have four components.");
    }
    return Quat{static_cast<float>(value.At(0).ToDouble()), static_cast<float>(value.At(1).ToDouble()),
        static_cast<float>(value.At(2).ToDouble()), static_cast<float>(value.At(3).ToDouble())};
}

float HalfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    const uint32_t exponent = (half >> 10) & 0x1F;
    uint32_t mantissa = half & 0x3FF;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: shift until the implicit bit appears.
            int shift = 0;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3FF;
            bits = sign | static_cast<uint32_t>(127 - 15 + 1 - shift) << 23 | mantissa << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000 | mantissa << 13;
    } else {
        bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    }
    return std::bit_cast<float>(bits);
}

Vec3 Normalize(Vec3 v) {
    const float length = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (length == 0) {
        return v;
    }
    return Vec3{v.X / length, v.Y / length, v.Z / length};
}

// VBIB.DecompressNormal: two bytes of an R8G8B8A8_UNORM compressed normal/tangent.
Vec3 DecompressNormalV1(float x, float y) {
    x -= 128;
    y -= 128;
    const float zsignbit = x < 0 ? 1.0f : 0.0f;
    const float tsignbit = y < 0 ? 1.0f : 0.0f;
    const float zsign = -(2 * zsignbit - 1);
    const float tsign = -(2 * tsignbit - 1);
    x = x * zsign - zsignbit;
    y = y * tsign - tsignbit;
    x -= 64;
    y -= 64;
    const float xsignbit = x < 0 ? 1.0f : 0.0f;
    const float ysignbit = y < 0 ? 1.0f : 0.0f;
    const float xsign = -(2 * xsignbit - 1);
    const float ysign = -(2 * ysignbit - 1);
    x = (x * xsign - xsignbit) / 63;
    y = (y * ysign - ysignbit) / 63;
    float z = 1 - x - y;
    const float inverse = 1 / std::sqrt(x * x + y * y + z * z);
    x *= inverse * xsign;
    y *= inverse * ysign;
    z *= inverse * zsign;
    return Vec3{x, y, z};
}

// VBIB.DecompressNormalTangents2: octahedral normal in bits 12..31 of an R32_UINT.
Vec3 DecompressNormalV2(uint32_t packed) {
    const float x = static_cast<float>((packed >> 12) & 0x3FF) / 1023.0f * 2 - 1;
    const float y = static_cast<float>((packed >> 22) & 0x3FF) / 1023.0f * 2 - 1;
    const float z = 1 - std::abs(x) - std::abs(y);
    Vec3 v{x, y, z};
    const float t = std::clamp(-z, 0.0f, 1.0f);
    v.X += v.X >= 0 ? -t : t;
    v.Y += v.Y >= 0 ? -t : t;
    return Normalize(v);
}

struct LayoutField {
    std::string Semantic;
    int SemanticIndex = 0;
    uint32_t Format = 0;
    uint32_t Offset = 0;
};

struct RawBuffer {
    uint32_t Count = 0;
    uint32_t Stride = 0;
    std::vector<LayoutField> Fields;
    std::vector<uint8_t> Data;
};

std::vector<uint8_t> Unzstd(std::span<const uint8_t> source, size_t capacity) {
    const unsigned long long content = ZSTD_getFrameContentSize(source.data(), source.size());
    if (content == ZSTD_CONTENTSIZE_ERROR) {
        throw InvalidData("Mesh buffer is not a Zstd frame.");
    }
    if (content != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (content > capacity) {
            throw InvalidData("Mesh buffer Zstd frame is larger than the buffer.");
        }
        capacity = static_cast<size_t>(content);
    }
    std::vector<uint8_t> out(capacity);
    const size_t written = ZSTD_decompress(out.data(), out.size(), source.data(), source.size());
    if (ZSTD_isError(written)) {
        throw InvalidData(std::string("Mesh buffer Zstd data is invalid: ") + ZSTD_getErrorName(written));
    }
    out.resize(written);
    return out;
}

RawBuffer ReadBuffer(const KvValue& description, const std::vector<Block>& blocks, bool is_vertex) {
    RawBuffer buffer;
    buffer.Count = static_cast<uint32_t>(Require(description, "m_nElementCount").ToInt32());
    buffer.Stride = static_cast<uint32_t>(Require(description, "m_nElementSizeInBytes").ToInt32());
    const uint64_t total = static_cast<uint64_t>(buffer.Count) * buffer.Stride;
    if (buffer.Stride == 0 || total > std::numeric_limits<int32_t>::max()) {
        throw InvalidData("Mesh buffer size is invalid.");
    }
    if (!is_vertex && buffer.Stride != 2 && buffer.Stride != 4) {
        throw InvalidData("Index buffer element size " + std::to_string(buffer.Stride) + " is not 2 or 4.");
    }

    if (const KvValue* fields = description.Find("m_inputLayoutFields"); fields != nullptr && fields->IsArray()) {
        for (const KvValue& item : fields->Items()) {
            LayoutField field;
            field.Semantic = Upper(StringOrBlob(Require(item, "m_pSemanticName")));
            field.SemanticIndex = Require(item, "m_nSemanticIndex").ToInt32();
            field.Format = static_cast<uint32_t>(Require(item, "m_Format").ToInt32());
            field.Offset = static_cast<uint32_t>(Require(item, "m_nOffset").ToInt32());
            buffer.Fields.push_back(std::move(field));
        }
    }

    std::span<const uint8_t> source;
    bool meshopt = false;
    bool zstd = false;
    if (const KvValue* inline_data = description.Find("m_pData"); inline_data != nullptr) {
        if (inline_data->Kind != KvKind::Blob) {
            throw InvalidData("Mesh buffer m_pData is not a blob.");
        }
        source = inline_data->Blob();
        meshopt = source.size() != total;
    } else {
        source = BlockAt(blocks, Require(description, "m_nBlockIndex").ToInt32()).Data;
        meshopt = FlagOr(description, "m_bMeshoptCompressed");
        zstd = FlagOr(description, "m_bCompressedZSTD");
    }

    std::vector<uint8_t> unzstd;
    if (zstd) {
        // Small meshoptimizer streams can be larger than the raw buffer, so allow up to the encoder's bound.
        size_t capacity = static_cast<size_t>(total);
        if (meshopt) {
            capacity = std::max(capacity, is_vertex
                    ? meshopt_encodeVertexBufferBound(buffer.Count, buffer.Stride)
                    : meshopt_encodeIndexBufferBound(buffer.Count, std::numeric_limits<uint32_t>::max()));
        }
        unzstd = Unzstd(source, capacity);
        source = unzstd;
    }

    buffer.Data.resize(static_cast<size_t>(total));
    if (!meshopt) {
        if (source.size() < total) {
            throw InvalidData("Mesh buffer is shorter than its element count.");
        }
        std::memcpy(buffer.Data.data(), source.data(), static_cast<size_t>(total));
        return buffer;
    }

    int result = 0;
    if (is_vertex) {
        if (buffer.Stride % 4 != 0 || buffer.Stride > 256) {
            throw InvalidData("Compressed vertex stride " + std::to_string(buffer.Stride) + " is not supported.");
        }
        result = meshopt_decodeVertexBuffer(buffer.Data.data(), buffer.Count, buffer.Stride, source.data(), source.size());
    } else if (FlagOr(description, "m_bMeshoptIndexSequence")) {
        result = meshopt_decodeIndexSequence(buffer.Data.data(), buffer.Count, buffer.Stride, source.data(), source.size());
    } else {
        if (buffer.Count % 3 != 0) {
            throw InvalidData("Compressed index buffer is not a triangle list.");
        }
        result = meshopt_decodeIndexBuffer(buffer.Data.data(), buffer.Count, buffer.Stride, source.data(), source.size());
    }
    if (result != 0) {
        throw InvalidData(std::string(is_vertex ? "Vertex" : "Index") + " buffer meshoptimizer data is invalid (" + std::to_string(result) + ").");
    }
    return buffer;
}

void CheckField(const RawBuffer& buffer, const LayoutField& field, uint32_t size) {
    if (static_cast<uint64_t>(field.Offset) + size > buffer.Stride) {
        throw InvalidData(field.Semantic + " does not fit in the vertex stride.");
    }
}

// Decoded influence before remapping. Up to eight slots so 8-bone formats can be trimmed after weighting.
struct Influences {
    std::array<uint16_t, 8> Index{};
    std::array<float, 8> Weight{};
    int Count = 0;
};

int ReadBlendIndices(const RawBuffer& buffer, const LayoutField& field, size_t vertex, std::array<uint16_t, 8>& out) {
    const uint8_t* at = buffer.Data.data() + vertex * buffer.Stride + field.Offset;
    switch (field.Format) {
    case R8G8B8A8_UINT:
        CheckField(buffer, field, 4);
        for (int i = 0; i < 4; ++i) {
            out[static_cast<size_t>(i)] = at[i];
        }
        return 4;
    case R16G16B16A16_UINT:
        // VBIB.GetBlendIndicesArray reads eight byte-sized indices from this format.
        CheckField(buffer, field, 8);
        for (int i = 0; i < 8; ++i) {
            out[static_cast<size_t>(i)] = at[i];
        }
        return 8;
    case R16G16_SINT:
        CheckField(buffer, field, 4);
        std::memcpy(out.data(), at, 4);
        return 2;
    case R16G16B16A16_SINT:
        CheckField(buffer, field, 8);
        std::memcpy(out.data(), at, 8);
        return 4;
    case R32G32B32A32_SINT:
        // Read as eight 16-bit indices, as VBIB.GetBlendIndicesArray does.
        CheckField(buffer, field, 16);
        std::memcpy(out.data(), at, 16);
        return 8;
    default:
        throw InvalidData("Unsupported BLENDINDICES format " + std::to_string(field.Format) + ".");
    }
}

int ReadBlendWeights(const RawBuffer& buffer, const LayoutField& field, size_t vertex, std::array<float, 8>& out) {
    const uint8_t* at = buffer.Data.data() + vertex * buffer.Stride + field.Offset;
    switch (field.Format) {
    case R8G8B8A8_UNORM:
        CheckField(buffer, field, 4);
        for (int i = 0; i < 4; ++i) {
            out[static_cast<size_t>(i)] = at[i] / 255.0f;
        }
        return 4;
    case R16G16B16A16_UNORM:
        // Eight byte-sized weights, matching VBIB.GetBlendWeightsArray.
        CheckField(buffer, field, 8);
        for (int i = 0; i < 8; ++i) {
            out[static_cast<size_t>(i)] = at[i] / 255.0f;
        }
        return 8;
    case R16G16_UNORM: {
        CheckField(buffer, field, 4);
        uint16_t values[2];
        std::memcpy(values, at, 4);
        out[0] = values[0] / 65535.0f;
        out[1] = values[1] / 65535.0f;
        return 2;
    }
    default:
        throw InvalidData("Unsupported BLENDWEIGHT format " + std::to_string(field.Format) + ".");
    }
}

GeometryVertexBuffer DecodeVertices(const RawBuffer& buffer, std::span<const int> remap, size_t bone_count) {
    GeometryVertexBuffer out;
    out.VertexCount = buffer.Count;
    const LayoutField* blend_indices = nullptr;
    const LayoutField* blend_weights = nullptr;

    for (const LayoutField& field : buffer.Fields) {
        if (field.SemanticIndex != 0) {
            continue;
        }
        if (field.Semantic == "POSITION") {
            if (field.Format != R32G32B32_FLOAT) {
                throw InvalidData("Unsupported POSITION format " + std::to_string(field.Format) + ".");
            }
            CheckField(buffer, field, 12);
            out.Positions.resize(buffer.Count);
            for (size_t v = 0; v < buffer.Count; ++v) {
                std::memcpy(&out.Positions[v], buffer.Data.data() + v * buffer.Stride + field.Offset, 12);
            }
        } else if (field.Semantic == "NORMAL") {
            out.Normals.resize(buffer.Count);
            for (size_t v = 0; v < buffer.Count; ++v) {
                const uint8_t* at = buffer.Data.data() + v * buffer.Stride + field.Offset;
                switch (field.Format) {
                case R32G32B32_FLOAT:
                    CheckField(buffer, field, 12);
                    std::memcpy(&out.Normals[v], at, 12);
                    break;
                case R32_UINT: {
                    CheckField(buffer, field, 4);
                    uint32_t packed;
                    std::memcpy(&packed, at, 4);
                    out.Normals[v] = DecompressNormalV2(packed);
                    break;
                }
                case R8G8B8A8_UNORM:
                    CheckField(buffer, field, 4);
                    out.Normals[v] = DecompressNormalV1(at[0], at[1]);
                    break;
                default:
                    throw InvalidData("Unsupported NORMAL format " + std::to_string(field.Format) + ".");
                }
            }
        } else if (field.Semantic == "TEXCOORD") {
            out.TexCoords.resize(buffer.Count);
            for (size_t v = 0; v < buffer.Count; ++v) {
                const uint8_t* at = buffer.Data.data() + v * buffer.Stride + field.Offset;
                switch (field.Format) {
                case R32G32_FLOAT:
                    CheckField(buffer, field, 8);
                    std::memcpy(&out.TexCoords[v], at, 8);
                    break;
                case R16G16_FLOAT: {
                    CheckField(buffer, field, 4);
                    uint16_t half[2];
                    std::memcpy(half, at, 4);
                    out.TexCoords[v] = Vec2{HalfToFloat(half[0]), HalfToFloat(half[1])};
                    break;
                }
                case R16G16_UNORM: {
                    CheckField(buffer, field, 4);
                    uint16_t value[2];
                    std::memcpy(value, at, 4);
                    out.TexCoords[v] = Vec2{value[0] / 65535.0f, value[1] / 65535.0f};
                    break;
                }
                case R16G16_SNORM: {
                    CheckField(buffer, field, 4);
                    int16_t value[2];
                    std::memcpy(value, at, 4);
                    out.TexCoords[v] = Vec2{value[0] / 32767.0f, value[1] / 32767.0f};
                    break;
                }
                default:
                    throw InvalidData("Unsupported TEXCOORD format " + std::to_string(field.Format) + ".");
                }
            }
        } else if (field.Semantic == "BLENDINDICES") {
            blend_indices = &field;
        } else if (field.Semantic == "BLENDWEIGHT" || field.Semantic == "BLENDWEIGHTS") {
            blend_weights = &field;
        }
    }

    if (blend_indices == nullptr || bone_count == 0) {
        return out;
    }
    out.BoneIndices.resize(buffer.Count);
    out.BoneWeights.resize(buffer.Count);
    for (size_t v = 0; v < buffer.Count; ++v) {
        Influences influence;
        influence.Count = ReadBlendIndices(buffer, *blend_indices, v, influence.Index);
        if (blend_weights != nullptr) {
            const int weights = ReadBlendWeights(buffer, *blend_weights, v, influence.Weight);
            influence.Count = std::min(influence.Count, weights);
        } else {
            influence.Count = 1;
            influence.Weight[0] = 1;
        }

        // Keep the four strongest influences; hero meshes use four, so this only trims eight-bone formats.
        std::array<int, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
        std::stable_sort(order.begin(), order.begin() + influence.Count,
            [&](int a, int b) { return influence.Weight[static_cast<size_t>(a)] > influence.Weight[static_cast<size_t>(b)]; });
        const int kept = std::min(influence.Count, 4);
        float sum = 0;
        for (int i = 0; i < kept; ++i) {
            sum += influence.Weight[static_cast<size_t>(order[static_cast<size_t>(i)])];
        }

        auto& indices = out.BoneIndices[v];
        auto& weights = out.BoneWeights[v];
        indices = {};
        weights = {};
        for (int i = 0; i < kept; ++i) {
            const size_t slot = static_cast<size_t>(order[static_cast<size_t>(i)]);
            const float weight = influence.Weight[slot];
            if (weight <= 0) {
                continue;
            }
            int bone = influence.Index[slot];
            if (!remap.empty()) {
                if (static_cast<size_t>(bone) >= remap.size()) {
                    throw InvalidData("Blend index " + std::to_string(bone) + " is outside the mesh remapping table.");
                }
                bone = remap[static_cast<size_t>(bone)];
            }
            if (bone < 0 || static_cast<size_t>(bone) >= bone_count) {
                throw InvalidData("Blend index points outside the model skeleton.");
            }
            indices[static_cast<size_t>(i)] = static_cast<uint16_t>(bone);
            weights[static_cast<size_t>(i)] = sum > 0 ? weight / sum : 0;
        }
    }
    return out;
}

std::vector<uint32_t> DecodeIndices(const RawBuffer& buffer) {
    std::vector<uint32_t> out(buffer.Count);
    if (buffer.Stride == 4) {
        std::memcpy(out.data(), buffer.Data.data(), buffer.Data.size());
    } else {
        for (size_t i = 0; i < buffer.Count; ++i) {
            out[i] = Read<uint16_t>(buffer.Data, i * 2);
        }
    }
    return out;
}

Quat Multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z,
    };
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // v + 2w(q x v) + 2(q x (q x v))
    const Vec3 u{q.X, q.Y, q.Z};
    const Vec3 t{2 * (u.Y * v.Z - u.Z * v.Y), 2 * (u.Z * v.X - u.X * v.Z), 2 * (u.X * v.Y - u.Y * v.X)};
    return Vec3{
        v.X + q.W * t.X + (u.Y * t.Z - u.Z * t.Y),
        v.Y + q.W * t.Y + (u.Z * t.X - u.X * t.Z),
        v.Z + q.W * t.Z + (u.X * t.Y - u.Y * t.X),
    };
}

std::vector<GeometryBone> DecodeBones(const KvValue& root) {
    const KvValue* skeleton = root.Find("m_modelSkeleton");
    if (skeleton == nullptr || !skeleton->IsObject()) {
        return {};
    }
    const KvValue& names = RequireArray(*skeleton, "m_boneName");
    const KvValue& parents = RequireArray(*skeleton, "m_nParent");
    const KvValue& positions = RequireArray(*skeleton, "m_bonePosParent");
    const KvValue& rotations = RequireArray(*skeleton, "m_boneRotParent");
    const size_t count = names.Count();
    if (parents.Count() != count || positions.Count() != count || rotations.Count() != count) {
        throw InvalidData("Model skeleton arrays have different lengths.");
    }

    std::vector<GeometryBone> bones(count);
    for (size_t i = 0; i < count; ++i) {
        GeometryBone& bone = bones[i];
        bone.Name = names.At(i).String();
        bone.Parent = parents.At(i).ToInt32();
        if (bone.Parent < -1 || bone.Parent >= static_cast<int>(count)) {
            throw InvalidData("Bone " + bone.Name + " has an out-of-range parent index.");
        }
        bone.LocalPosition = ReadVec3(positions.At(i));
        bone.LocalRotation = ReadQuat(rotations.At(i));
    }

    // Parents normally precede children, but resolve in dependency order so any order works.
    std::vector<uint8_t> state(count, 0);  // 0 = pending, 1 = visiting, 2 = done
    for (size_t start = 0; start < count; ++start) {
        std::vector<size_t> chain;
        size_t at = start;
        while (state[at] != 2) {
            if (state[at] == 1) {
                throw InvalidData("Skeleton has a parent cycle at " + bones[at].Name + ".");
            }
            state[at] = 1;
            chain.push_back(at);
            if (bones[at].Parent < 0) {
                break;
            }
            at = static_cast<size_t>(bones[at].Parent);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            GeometryBone& bone = bones[*it];
            if (bone.Parent < 0) {
                bone.ModelPosition = bone.LocalPosition;
                bone.ModelRotation = bone.LocalRotation;
            } else {
                const GeometryBone& parent = bones[static_cast<size_t>(bone.Parent)];
                const Vec3 offset = Rotate(parent.ModelRotation, bone.LocalPosition);
                bone.ModelPosition = Vec3{parent.ModelPosition.X + offset.X, parent.ModelPosition.Y + offset.Y,
                    parent.ModelPosition.Z + offset.Z};
                bone.ModelRotation = Multiply(parent.ModelRotation, bone.LocalRotation);
            }
            state[*it] = 2;
        }
    }
    return bones;
}

std::vector<int> RemapTable(const KvValue& root, int mesh_index) {
    const KvValue* starts = root.Find("m_remappingTableStarts");
    const KvValue* table = root.Find("m_remappingTable");
    if (starts == nullptr || table == nullptr || !starts->IsArray() || !table->IsArray()
        || mesh_index < 0 || static_cast<size_t>(mesh_index) >= starts->Count()) {
        return {};
    }
    const int64_t begin = starts->At(static_cast<size_t>(mesh_index)).ToInt32();
    const int64_t end = static_cast<size_t>(mesh_index) + 1 < starts->Count()
        ? starts->At(static_cast<size_t>(mesh_index) + 1).ToInt32()
        : static_cast<int64_t>(table->Count());
    if (begin < 0 || end < begin || end > static_cast<int64_t>(table->Count())) {
        throw InvalidData("Mesh remapping table range is invalid.");
    }
    std::vector<int> remap;
    remap.reserve(static_cast<size_t>(end - begin));
    for (int64_t i = begin; i < end; ++i) {
        remap.push_back(table->At(static_cast<size_t>(i)).ToInt32());
    }
    return remap;
}

uint64_t MaskAt(const KvValue& root, std::string_view key, int index) {
    const KvValue* masks = root.Find(key);
    if (masks == nullptr || !masks->IsArray() || index < 0 || static_cast<size_t>(index) >= masks->Count()) {
        return 0;
    }
    return ToUInt64(masks->At(static_cast<size_t>(index)));
}

uint32_t BufferHandle(const KvValue& binding, size_t buffer_count, const char* what) {
    const int handle = Require(binding, "m_hBuffer").ToInt32();
    if (handle < 0 || static_cast<size_t>(handle) >= buffer_count) {
        throw InvalidData(std::string("Draw call ") + what + " buffer " + std::to_string(handle) + " does not exist.");
    }
    return static_cast<uint32_t>(handle);
}

void DecodeDrawCalls(const KvValue& mesh_data, GeometryMesh& mesh, const std::vector<RawBuffer>& vertex_raw,
    const std::vector<RawBuffer>& index_raw) {
    const KvValue& scene_objects = RequireArray(mesh_data, "m_sceneObjects");
    bool first_bounds = true;
    for (const KvValue& scene_object : scene_objects.Items()) {
        if (const KvValue* min = scene_object.Find("m_vMinBounds"), *max = scene_object.Find("m_vMaxBounds"); min != nullptr && max != nullptr) {
            const Vec3 lo = ReadVec3(*min);
            const Vec3 hi = ReadVec3(*max);
            if (first_bounds) {
                mesh.MinBounds = lo;
                mesh.MaxBounds = hi;
                first_bounds = false;
            } else {
                mesh.MinBounds = Vec3{std::min(mesh.MinBounds.X, lo.X), std::min(mesh.MinBounds.Y, lo.Y), std::min(mesh.MinBounds.Z, lo.Z)};
                mesh.MaxBounds = Vec3{std::max(mesh.MaxBounds.X, hi.X), std::max(mesh.MaxBounds.Y, hi.Y), std::max(mesh.MaxBounds.Z, hi.Z)};
            }
        }

        for (const KvValue& call : RequireArray(scene_object, "m_drawCalls").Items()) {
            if (const KvValue* primitive = call.Find("m_nPrimitiveType");
                primitive != nullptr && primitive->Kind == KvKind::String && primitive->String() != "RENDER_PRIM_TRIANGLES") {
                continue;
            }
            GeometryDrawCall draw;
            const KvValue& vertex_bindings = RequireArray(call, "m_vertexBuffers");
            if (vertex_bindings.Count() == 0) {
                throw InvalidData("Draw call has no vertex buffer.");
            }
            draw.VertexBuffer = BufferHandle(vertex_bindings.At(0), vertex_raw.size(), "vertex");
            const KvValue& index_binding = Require(call, "m_indexBuffer");
            draw.IndexBuffer = BufferHandle(index_binding, index_raw.size(), "index");

            const RawBuffer& vertices = vertex_raw[draw.VertexBuffer];
            const RawBuffer& indices = index_raw[draw.IndexBuffer];
            const int vertex_offset = IntOr(vertex_bindings.At(0), "m_nBindOffsetBytes", 0);
            const int index_offset = IntOr(index_binding, "m_nBindOffsetBytes", 0);
            if (vertex_offset < 0 || index_offset < 0 || vertex_offset % static_cast<int>(vertices.Stride) != 0
                || index_offset % static_cast<int>(indices.Stride) != 0) {
                throw InvalidData("Draw call buffer offset is not aligned to its element size.");
            }

            const int64_t start = static_cast<int64_t>(Require(call, "m_nStartIndex").ToInt32()) + index_offset / static_cast<int>(indices.Stride);
            const int64_t count = Require(call, "m_nIndexCount").ToInt32();
            const int64_t base = static_cast<int64_t>(IntOr(call, "m_nBaseVertex", 0)) + vertex_offset / static_cast<int>(vertices.Stride);
            if (start < 0 || count < 0 || start + count > indices.Count) {
                throw InvalidData("Draw call index range is outside the index buffer.");
            }
            if (base < 0 || base > vertices.Count) {
                throw InvalidData("Draw call base vertex is outside the vertex buffer.");
            }
            draw.StartIndex = static_cast<uint32_t>(start);
            draw.IndexCount = static_cast<uint32_t>(count);
            draw.BaseVertex = static_cast<int32_t>(base);
            if (const KvValue* material = call.Find("m_material"); material != nullptr && material->Kind == KvKind::String) {
                draw.Material = material->String();
            }
            mesh.DrawCalls.push_back(std::move(draw));
        }
    }
}

void CheckDrawRanges(const GeometryMesh& mesh) {
    for (const GeometryDrawCall& draw : mesh.DrawCalls) {
        const auto& indices = mesh.IndexBuffers[draw.IndexBuffer];
        const uint32_t vertex_count = mesh.VertexBuffers[draw.VertexBuffer].VertexCount;
        for (uint32_t i = draw.StartIndex; i < draw.StartIndex + draw.IndexCount; ++i) {
            if (static_cast<uint64_t>(indices[i]) + static_cast<uint64_t>(draw.BaseVertex) >= vertex_count) {
                throw InvalidData("Mesh " + mesh.Name + " has an index outside its vertex buffer.");
            }
        }
    }
}

}  // namespace

ModelGeometry DecodeModelGeometry(std::span<const uint8_t> file) {
    const std::vector<Block> blocks = ReadBlocks(file);
    const Block* data_block = FindBlock(blocks, "DATA");
    if (data_block == nullptr) {
        throw InvalidData("Resource has no DATA block.");
    }
    const KvValue root = ReadKvBlock(*data_block);

    ModelGeometry geometry;
    geometry.Bones = DecodeBones(root);
    if (const KvValue* mask = root.Find("m_nDefaultMeshGroupMask")) {
        geometry.DefaultMeshGroupMask = ToUInt64(*mask);
    }
    if (const KvValue* refs = root.Find("m_refMeshes"); refs != nullptr && refs->IsArray()) {
        for (const KvValue& ref : refs->Items()) {
            if (ref.Kind == KvKind::String && !ref.String().empty()) {
                geometry.ExternalMeshes.push_back(ref.String());
            }
        }
    }

    const Block* control = FindBlock(blocks, "CTRL");
    if (control == nullptr) {
        return geometry;
    }
    const KvValue control_root = ReadKvBlock(*control);
    const KvValue* embedded = control_root.Find("embedded_meshes");
    if (embedded == nullptr || !embedded->IsArray()) {
        return geometry;
    }

    for (const KvValue& entry : embedded->Items()) {
        if (entry.Contains("vbib_block")) {
            throw InvalidData("Models with a separate VBIB block are not supported yet.");
        }
        GeometryMesh mesh;
        mesh.Name = Require(entry, "m_Name").String();
        mesh.MeshIndex = Require(entry, "m_nMeshIndex").ToInt32();
        mesh.LodMask = MaskAt(root, "m_refLODGroupMasks", mesh.MeshIndex);
        mesh.MeshGroupMask = MaskAt(root, "m_refMeshGroupMasks", mesh.MeshIndex);

        std::vector<RawBuffer> vertex_raw;
        for (const KvValue& description : RequireArray(entry, "m_vertexBuffers").Items()) {
            vertex_raw.push_back(ReadBuffer(description, blocks, true));
        }
        std::vector<RawBuffer> index_raw;
        for (const KvValue& description : RequireArray(entry, "m_indexBuffers").Items()) {
            index_raw.push_back(ReadBuffer(description, blocks, false));
        }

        const KvValue mesh_data = ReadKvBlock(BlockAt(blocks, Require(entry, "m_nDataBlock").ToInt32()));
        DecodeDrawCalls(mesh_data, mesh, vertex_raw, index_raw);

        const std::vector<int> remap = RemapTable(root, mesh.MeshIndex);
        for (const RawBuffer& raw : vertex_raw) {
            mesh.VertexBuffers.push_back(DecodeVertices(raw, remap, geometry.Bones.size()));
        }
        for (const RawBuffer& raw : index_raw) {
            mesh.IndexBuffers.push_back(DecodeIndices(raw));
        }
        CheckDrawRanges(mesh);
        geometry.Meshes.push_back(std::move(mesh));
    }
    return geometry;
}

}  // namespace skeleton_repair
