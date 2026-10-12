#include "kv3.hpp"

#include "error.hpp"

#include <lz4.h>
#include <zstd.h>

#include <bit>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

namespace skeleton_repair {
namespace {

constexpr uint32_t kMagic0 = 0x03564B56u;
constexpr uint32_t kMagicBase = 0x4B563300u;
constexpr uint32_t kTrailer = 0xFFEEDD00u;
constexpr int kLz4Frame = 16384;
constexpr int kMaxDepth = 256;

enum class Node : uint8_t {
    Null = 1,
    Boolean = 2,
    Int64 = 3,
    UInt64 = 4,
    Double = 5,
    String = 6,
    Blob = 7,
    Array = 8,
    Object = 9,
    ArrayTyped = 10,
    Int32 = 11,
    UInt32 = 12,
    BoolTrue = 13,
    BoolFalse = 14,
    Int64Zero = 15,
    Int64One = 16,
    DoubleZero = 17,
    DoubleOne = 18,
    Float = 19,
    Int16 = 20,
    UInt16 = 21,
    Int32AsByte = 23,
    ArrayByteLength = 24,
    ArrayAux = 25,
};

enum class Compression : uint32_t { None = 0, Lz4 = 1, Zstd = 2 };

struct Lane {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t i = 0;

    size_t Left() const { return n - i; }
    bool Done() const { return i == n; }

    void Need(size_t count) const {
        if (count > Left()) {
            throw InvalidData("KV3 data ended early.");
        }
    }

    template <typename T>
    T Take() {
        Need(sizeof(T));
        T value;
        std::memcpy(&value, p + i, sizeof(T));
        i += sizeof(T);
        return value;
    }

    std::span<const uint8_t> TakeBytes(size_t count) {
        Need(count);
        std::span<const uint8_t> out(p + i, count);
        i += count;
        return out;
    }

    std::string TakeString() {
        const size_t start = i;
        while (i < n && p[i] != 0) {
            ++i;
        }
        if (i >= n) {
            throw InvalidData("KV3 string is not terminated.");
        }
        std::string text(reinterpret_cast<const char*>(p + start), i - start);
        ++i;
        return text;
    }
};

struct Lanes {
    Lane b1;
    Lane b2;
    Lane b4;
    Lane b8;
};

struct Context {
    int version = 5;
    Lane types;
    Lane object_lengths;
    Lane blob_lengths;
    Lane blobs;
    std::vector<std::string> strings;
    Lanes buf;
    Lanes aux;
};

struct Buf {
    std::vector<uint8_t> b;

    void Add(const void* data, size_t n) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        b.insert(b.end(), bytes, bytes + n);
    }

    template <typename T>
    void Put(T value) {
        Add(&value, sizeof(T));
    }
};

void Exhaust(const Lane& lane, const char* what) {
    if (!lane.Done()) {
        throw InvalidData(std::string("KV3 ") + what + " was not fully consumed.");
    }
}

int Fit(size_t n, const char* what) {
    if (n > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw InvalidData(std::string("KV3 ") + what + " does not fit.");
    }
    return static_cast<int>(n);
}

size_t Mul(int count, int width) {
    if (count < 0) {
        throw InvalidData("KV3 count is negative.");
    }
    if (width <= 0 || static_cast<size_t>(count) > SIZE_MAX / static_cast<size_t>(width)) {
        throw InvalidData("KV3 count is too large.");
    }
    return static_cast<size_t>(count) * static_cast<size_t>(width);
}

void Align(size_t& offset, size_t size, int alignment) {
    const size_t mask = static_cast<size_t>(alignment) - 1;
    const size_t aligned = (offset + mask) & ~mask;
    if (aligned > size) {
        throw InvalidData("KV3 buffer is truncated.");
    }
    offset = aligned;
}

Lane TakeLane(const uint8_t* base, size_t size, size_t& offset, int count, int width, int alignment) {
    if (count == 0) {
        return {};
    }
    const size_t bytes = Mul(count, width);
    if (alignment > 1) {
        Align(offset, size, alignment);
    }
    if (bytes > size - offset) {
        throw InvalidData("KV3 buffer is truncated.");
    }
    Lane lane{base + offset, bytes, 0};
    offset += bytes;
    return lane;
}

std::vector<uint8_t> DecompressLz4(std::span<const uint8_t> input, int output_size) {
    if (output_size < 0) {
        throw InvalidData("KV3 LZ4 size is negative.");
    }
    std::vector<uint8_t> output(static_cast<size_t>(output_size));
    const int written = LZ4_decompress_safe(reinterpret_cast<const char*>(input.data()),
        reinterpret_cast<char*>(output.data()), Fit(input.size(), "LZ4 input"), output_size);
    if (written != output_size) {
        throw InvalidData("Failed to decompress LZ4 KV3 data.");
    }
    return output;
}

std::vector<uint8_t> DecompressZstd(std::span<const uint8_t> input, int output_size) {
    if (output_size < 0) {
        throw InvalidData("KV3 Zstd size is negative.");
    }
    std::vector<uint8_t> output(static_cast<size_t>(output_size));
    const size_t written = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(written) || written != output.size()) {
        throw InvalidData("Failed to decompress Zstd KV3 data.");
    }
    return output;
}

std::vector<uint8_t> DecompressLz4Chain(Lane& sizes, Lane& compressed, int frame, int output_size) {
    if (frame <= 0 || output_size < 0) {
        throw InvalidData("KV3 LZ4 blob frame is invalid.");
    }
    std::vector<uint8_t> output(static_cast<size_t>(output_size));
    LZ4_streamDecode_t stream{};
    size_t produced = 0;
    while (!sizes.Done()) {
        const int compressed_size = sizes.Take<uint16_t>();
        if (compressed_size <= 0) {
            throw InvalidData("KV3 LZ4 blob frame is empty.");
        }
        const auto input = compressed.TakeBytes(static_cast<size_t>(compressed_size));
        if (produced > output.size()) {
            throw InvalidData("KV3 LZ4 blob overran its buffer.");
        }
        const int room = static_cast<int>(std::min(static_cast<size_t>(frame), output.size() - produced));
        const int decoded = LZ4_decompress_safe_continue(&stream, reinterpret_cast<const char*>(input.data()),
            reinterpret_cast<char*>(output.data() + produced), compressed_size, room);
        if (decoded != room) {
            throw InvalidData("Failed to decompress an LZ4 KV3 blob.");
        }
        produced += static_cast<size_t>(decoded);
    }
    if (produced != output.size()) {
        throw InvalidData("KV3 LZ4 blob size does not match.");
    }
    return output;
}

std::vector<uint8_t> ReadCompressed(Lane& file, Compression compression, int compressed_size, int uncompressed_size) {
    if (compression == Compression::None) {
        auto bytes = file.TakeBytes(static_cast<size_t>(uncompressed_size));
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
    if (compressed_size <= 0) {
        throw InvalidData("KV3 compressed size is invalid.");
    }
    const auto input = file.TakeBytes(static_cast<size_t>(compressed_size));
    if (compression == Compression::Lz4) {
        return DecompressLz4(input, uncompressed_size);
    }
    return DecompressZstd(input, uncompressed_size);
}

const std::string& StringAt(const Context& ctx, int32_t id) {
    static const std::string empty;
    if (id == -1) {
        return empty;
    }
    if (id < 0 || static_cast<size_t>(id) >= ctx.strings.size()) {
        throw InvalidData("KV3 string id is out of range.");
    }
    return ctx.strings[static_cast<size_t>(id)];
}

std::pair<Node, KvFlag> ReadType(Context& ctx) {
    uint8_t byte = ctx.types.Take<uint8_t>();
    KvFlag flag = KvFlag::None;
    if (ctx.version >= 3) {
        if ((byte & 0x80) != 0) {
            byte &= 0x3F;
            const uint8_t raw = ctx.types.Take<uint8_t>();
            if (raw > static_cast<uint8_t>(KvFlag::EntityName)) {
                throw InvalidData("KV3 flag is not supported.");
            }
            flag = static_cast<KvFlag>(raw);
        }
    } else if ((byte & 0x80) != 0) {
        byte &= 0x7F;
        uint8_t raw = ctx.types.Take<uint8_t>();
        if ((raw & 4) != 0) {
            raw = static_cast<uint8_t>(raw ^ 4);
        }
        switch (raw) {
        case 0: flag = KvFlag::None; break;
        case 1: flag = KvFlag::Resource; break;
        case 2: flag = KvFlag::ResourceName; break;
        case 8: flag = KvFlag::Panorama; break;
        case 16: flag = KvFlag::SoundEvent; break;
        case 32: flag = KvFlag::SubClass; break;
        default: throw InvalidData("KV3 flag is not supported.");
        }
    }

    switch (static_cast<Node>(byte)) {
    case Node::Null:
    case Node::Boolean:
    case Node::Int64:
    case Node::UInt64:
    case Node::Double:
    case Node::String:
    case Node::Blob:
    case Node::Array:
    case Node::Object:
    case Node::ArrayTyped:
    case Node::Int32:
    case Node::UInt32:
    case Node::BoolTrue:
    case Node::BoolFalse:
    case Node::Int64Zero:
    case Node::Int64One:
    case Node::DoubleZero:
    case Node::DoubleOne:
    case Node::Float:
    case Node::Int16:
    case Node::UInt16:
    case Node::Int32AsByte:
    case Node::ArrayByteLength:
    case Node::ArrayAux:
        return {static_cast<Node>(byte), flag};
    default:
        throw InvalidData("KV3 value type is not supported.");
    }
}

KvValue ReadNode(Context& ctx, Node type, KvFlag flag, int depth);

KvValue ReadPayload(Context& ctx, Node type, int depth) {
    if (depth > kMaxDepth) {
        throw InvalidData("KV3 value is nested too deeply.");
    }
    Lanes& buf = ctx.buf;
    switch (type) {
    case Node::Null:
        return KvValue::Null();
    case Node::BoolTrue:
        return KvValue::Bool(true);
    case Node::BoolFalse:
        return KvValue::Bool(false);
    case Node::Int64Zero:
        return KvValue::Int64(0);
    case Node::Int64One:
        return KvValue::Int64(1);
    case Node::DoubleZero:
        return KvValue::Double(0.0);
    case Node::DoubleOne:
        return KvValue::Double(1.0);
    case Node::Boolean:
        return KvValue::Bool(buf.b1.Take<uint8_t>() == 1);
    case Node::Int32AsByte:
        return KvValue::Int32(buf.b1.Take<uint8_t>());
    case Node::Int16:
        return KvValue::Int16(buf.b2.Take<int16_t>());
    case Node::UInt16:
        return KvValue::UInt16(buf.b2.Take<uint16_t>());
    case Node::Int32:
        return KvValue::Int32(buf.b4.Take<int32_t>());
    case Node::UInt32:
        return KvValue::UInt32(buf.b4.Take<uint32_t>());
    case Node::Float:
        return KvValue::Float(buf.b4.Take<float>());
    case Node::Int64:
        return KvValue::Int64(buf.b8.Take<int64_t>());
    case Node::UInt64:
        return KvValue::UInt64(buf.b8.Take<uint64_t>());
    case Node::Double:
        return KvValue::Double(buf.b8.Take<double>());
    case Node::String:
        return KvValue::String(StringAt(ctx, buf.b4.Take<int32_t>()));
    case Node::Blob: {
        int length = 0;
        std::span<const uint8_t> bytes;
        if (ctx.version < 2) {
            length = buf.b4.Take<int32_t>();
            if (length < 0) {
                throw InvalidData("KV3 blob length is negative.");
            }
            bytes = length == 0 ? std::span<const uint8_t>{} : buf.b1.TakeBytes(static_cast<size_t>(length));
        } else {
            length = ctx.blob_lengths.Take<int32_t>();
            if (length < 0) {
                throw InvalidData("KV3 blob length is negative.");
            }
            bytes = length == 0 ? std::span<const uint8_t>{} : ctx.blobs.TakeBytes(static_cast<size_t>(length));
        }
        return KvValue::Blob(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
    case Node::Array: {
        const int count = buf.b4.Take<int32_t>();
        if (count < 0) {
            throw InvalidData("KV3 array length is negative.");
        }
        std::vector<KvValue> items;
        items.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto [child_type, child_flag] = ReadType(ctx);
            items.push_back(ReadNode(ctx, child_type, child_flag, depth + 1));
        }
        return KvValue::Array(std::move(items));
    }
    case Node::ArrayTyped:
    case Node::ArrayByteLength:
    case Node::ArrayAux: {
        int count = 0;
        if (type == Node::ArrayByteLength || type == Node::ArrayAux) {
            count = buf.b1.Take<uint8_t>();
        } else {
            count = buf.b4.Take<int32_t>();
            if (count < 0) {
                throw InvalidData("KV3 array length is negative.");
            }
        }
        const auto [child_type, child_flag] = ReadType(ctx);
        const bool auxiliary = type == Node::ArrayAux;
        if (auxiliary) {
            std::swap(ctx.buf, ctx.aux);
        }
        std::vector<KvValue> items;
        items.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            items.push_back(ReadNode(ctx, child_type, child_flag, depth + 1));
        }
        if (auxiliary) {
            std::swap(ctx.buf, ctx.aux);
        }
        return KvValue::Array(std::move(items));
    }
    case Node::Object: {
        int count = 0;
        if (ctx.version >= 5) {
            count = ctx.object_lengths.Take<int32_t>();
        } else {
            count = buf.b4.Take<int32_t>();
        }
        if (count < 0) {
            throw InvalidData("KV3 object length is negative.");
        }
        std::vector<std::pair<std::string, KvValue>> fields;
        fields.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto [child_type, child_flag] = ReadType(ctx);
            const int32_t name_id = ctx.buf.b4.Take<int32_t>();
            fields.emplace_back(StringAt(ctx, name_id), ReadNode(ctx, child_type, child_flag, depth + 1));
        }
        return KvValue::Object(std::move(fields));
    }
    }
    throw InvalidData("KV3 value type is not supported.");
}

KvValue ReadNode(Context& ctx, Node type, KvFlag flag, int depth) {
    KvValue value = ReadPayload(ctx, type, depth);
    value.Flag = flag;
    return value;
}

void ReadStrings(Lane& bytes, int count, std::vector<std::string>& strings) {
    if (count < 0) {
        throw InvalidData("KV3 string count is negative.");
    }
    strings.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        strings.push_back(bytes.TakeString());
    }
}

void SplitLeadingLanes(const std::vector<uint8_t>& buffer, int version, int count1, int count2, int count4, int count8,
    Lanes& lanes, size_t& offset) {
    const auto* base = buffer.data();
    const size_t size = buffer.size();
    offset = 0;
    lanes.b1 = TakeLane(base, size, offset, count1, 1, 1);
    lanes.b2 = TakeLane(base, size, offset, count2, 2, 2);
    lanes.b4 = TakeLane(base, size, offset, count4, 4, 4);
    if (count8 > 0) {
        lanes.b8 = TakeLane(base, size, offset, count8, 8, 8);
    } else if (version < 5) {
        Align(offset, size, 8);
    }
}

struct Header {
    int version = 0;
    std::array<uint8_t, 16> format{};
    Compression compression = Compression::None;
    int frame = 0;
    int count1 = 0;
    int count2 = 0;
    int count4 = 0;
    int count8 = 0;
    int count_types = 0;
    int size_uncompressed = 0;
    int size_compressed = 0;
    int count_blocks = 0;
    int size_blobs = 0;
    int size_block_sizes = 0;
    int unc1 = 0;
    int comp1 = 0;
    int unc2 = 0;
    int comp2 = 0;
    int b2_count1 = 0;
    int b2_count2 = 0;
    int b2_count4 = 0;
    int b2_count8 = 0;
    int objects2 = 0;
};

int NeedCount(int value, const char* what) {
    if (value < 0) {
        throw InvalidData(std::string("KV3 ") + what + " is negative.");
    }
    return value;
}

Header ReadHeader(Lane& file) {
    if (file.Left() < 4) {
        throw InvalidData("KV3 data ended early.");
    }
    const uint32_t magic = file.Take<uint32_t>();
    if (magic == kMagic0) {
        throw InvalidData("KV3 version 0 is not supported.");
    }
    if ((magic & 0xFFFFFF00u) != kMagicBase || (magic & 0xFFu) < 1 || (magic & 0xFFu) > 5) {
        throw InvalidData("Unsupported KV3 signature.");
    }

    Header header;
    header.version = static_cast<int>(magic & 0xFFu);
    const auto guid = file.TakeBytes(16);
    std::memcpy(header.format.data(), guid.data(), 16);
    const auto compression = file.Take<uint32_t>();
    if (compression > static_cast<uint32_t>(Compression::Zstd)) {
        throw InvalidData("KV3 compression method is not supported.");
    }
    header.compression = static_cast<Compression>(compression);

    if (header.version == 1) {
        header.count1 = NeedCount(file.Take<int32_t>(), "byte count");
        header.count4 = NeedCount(file.Take<int32_t>(), "dword count");
        header.count8 = NeedCount(file.Take<int32_t>(), "qword count");
        header.size_uncompressed = NeedCount(file.Take<int32_t>(), "buffer size");
        header.size_compressed = Fit(file.Left(), "compressed size");
    } else {
        const int dictionary = file.Take<uint16_t>();
        header.frame = file.Take<uint16_t>();
        if (dictionary != 0) {
            throw InvalidData("KV3 compression dictionary is not supported.");
        }
        if (header.compression == Compression::Lz4) {
            if (header.frame != kLz4Frame) {
                throw InvalidData("KV3 LZ4 frame size is not supported.");
            }
        } else if (header.frame != 0) {
            throw InvalidData("KV3 compression frame is not supported.");
        }
        header.count1 = NeedCount(file.Take<int32_t>(), "byte count");
        header.count4 = NeedCount(file.Take<int32_t>(), "dword count");
        header.count8 = NeedCount(file.Take<int32_t>(), "qword count");
        header.count_types = NeedCount(file.Take<int32_t>(), "type count");
        file.Take<uint16_t>();
        file.Take<uint16_t>();
        header.size_uncompressed = NeedCount(file.Take<int32_t>(), "buffer size");
        header.size_compressed = NeedCount(file.Take<int32_t>(), "compressed size");
        header.count_blocks = NeedCount(file.Take<int32_t>(), "blob count");
        header.size_blobs = NeedCount(file.Take<int32_t>(), "blob bytes");
    }

    if (header.version >= 4) {
        header.count2 = NeedCount(file.Take<int32_t>(), "word count");
        header.size_block_sizes = NeedCount(file.Take<int32_t>(), "blob frame table");
    }
    if (header.version >= 5) {
        header.unc1 = NeedCount(file.Take<int32_t>(), "buffer size");
        header.comp1 = NeedCount(file.Take<int32_t>(), "compressed size");
        header.unc2 = NeedCount(file.Take<int32_t>(), "buffer size");
        header.comp2 = NeedCount(file.Take<int32_t>(), "compressed size");
        header.b2_count1 = NeedCount(file.Take<int32_t>(), "byte count");
        header.b2_count2 = NeedCount(file.Take<int32_t>(), "word count");
        header.b2_count4 = NeedCount(file.Take<int32_t>(), "dword count");
        header.b2_count8 = NeedCount(file.Take<int32_t>(), "qword count");
        file.Take<int32_t>();
        header.objects2 = NeedCount(file.Take<int32_t>(), "object count");
        file.Take<int32_t>();
        file.Take<int32_t>();
        if (static_cast<long long>(header.size_uncompressed) != static_cast<long long>(header.unc1) + header.unc2) {
            throw InvalidData("KV3 buffer sizes do not add up.");
        }
    } else {
        header.unc1 = header.size_uncompressed;
        header.comp1 = header.size_compressed;
    }
    return header;
}

void CheckTrailer(uint32_t value) {
    if (value != kTrailer) {
        throw InvalidData("KV3 block trailer is missing.");
    }
}

struct Written {
    Buf bytes1;
    Buf bytes2;
    Buf bytes4;
    Buf bytes8;
    Buf types;
    Buf object_lengths;
    Buf blobs;
    std::vector<int> blob_lengths;
    int arrays = 0;
    std::vector<std::string> strings;
    std::unordered_map<std::string, int32_t> string_ids;
    bool version5 = true;

    int32_t StringId(const std::string& text) {
        if (text.empty()) {
            return -1;
        }
        if (text.find('\0') != std::string::npos) {
            throw InvalidData("KV3 string contains a null character.");
        }
        if (const auto found = string_ids.find(text); found != string_ids.end()) {
            return found->second;
        }
        const int32_t id = Fit(strings.size(), "string count");
        strings.push_back(text);
        string_ids.emplace(strings.back(), id);
        return id;
    }
};

void WriteType(Written& out, Node type, KvFlag flag) {
    if (flag != KvFlag::None) {
        out.types.Put(static_cast<uint8_t>(static_cast<uint8_t>(type) | 0x80));
        out.types.Put(static_cast<uint8_t>(flag));
    } else {
        out.types.Put(static_cast<uint8_t>(type));
    }
}

void WriteValue(Written& out, const KvValue& value) {
    if (value.Kind == KvKind::Bool) {
        WriteType(out, value.Bool() ? Node::BoolTrue : Node::BoolFalse, value.Flag);
        return;
    }
    if (value.Kind == KvKind::Int64) {
        const int64_t number = value.Int64();
        if (number == 0) {
            WriteType(out, Node::Int64Zero, value.Flag);
            return;
        }
        if (number == 1) {
            WriteType(out, Node::Int64One, value.Flag);
            return;
        }
        WriteType(out, Node::Int64, value.Flag);
        out.bytes8.Put(number);
        return;
    }
    if (value.Kind == KvKind::Double) {
        const double number = value.Double();
        if (std::bit_cast<int64_t>(number) == 0) {
            WriteType(out, Node::DoubleZero, value.Flag);
            return;
        }
        if (number == 1.0) {
            WriteType(out, Node::DoubleOne, value.Flag);
            return;
        }
        WriteType(out, Node::Double, value.Flag);
        out.bytes8.Put(number);
        return;
    }

    Node type = Node::Null;
    switch (value.Kind) {
    case KvKind::Null: type = Node::Null; break;
    case KvKind::Int16: type = Node::Int16; break;
    case KvKind::UInt16: type = Node::UInt16; break;
    case KvKind::Int32: type = Node::Int32; break;
    case KvKind::UInt32: type = Node::UInt32; break;
    case KvKind::UInt64: type = Node::UInt64; break;
    case KvKind::Float: type = Node::Float; break;
    case KvKind::String: type = Node::String; break;
    case KvKind::Blob: type = Node::Blob; break;
    case KvKind::Array: type = Node::Array; break;
    case KvKind::Object: type = Node::Object; break;
    default: throw InvalidData("KV3 value type cannot be written.");
    }
    WriteType(out, type, value.Flag);

    switch (value.Kind) {
    case KvKind::Null:
        break;
    case KvKind::Int16:
        out.bytes2.Put(value.Int16());
        break;
    case KvKind::UInt16:
        out.bytes2.Put(value.UInt16());
        break;
    case KvKind::Int32:
        out.bytes4.Put(value.Int32());
        break;
    case KvKind::UInt32:
        out.bytes4.Put(value.UInt32());
        break;
    case KvKind::UInt64:
        out.bytes8.Put(value.UInt64());
        break;
    case KvKind::Float:
        out.bytes4.Put(value.Float());
        break;
    case KvKind::String:
        out.bytes4.Put(out.StringId(value.String()));
        break;
    case KvKind::Blob: {
        const auto& blob = value.Blob();
        out.blob_lengths.push_back(Fit(blob.size(), "blob"));
        out.blobs.Add(blob.data(), blob.size());
        break;
    }
    case KvKind::Object: {
        const int count = Fit(value.Fields().size(), "object");
        if (out.version5) {
            out.object_lengths.Put(count);
        } else {
            out.bytes4.Put(count);
        }
        for (const auto& [key, child] : value.Fields()) {
            out.bytes4.Put(out.StringId(key));
            WriteValue(out, child);
        }
        break;
    }
    case KvKind::Array: {
        ++out.arrays;
        out.bytes4.Put(Fit(value.Items().size(), "array"));
        for (const auto& child : value.Items()) {
            WriteValue(out, child);
        }
        break;
    }
    default:
        break;
    }
}

void AlignPad(Buf& dest, int& offset, int alignment) {
    const int mask = alignment - 1;
    const int aligned = (offset + mask) & ~mask;
    for (int i = offset; i < aligned; ++i) {
        dest.Put(static_cast<uint8_t>(0));
    }
    offset = aligned;
}

void WriteLane(Buf& dest, int& offset, const std::vector<uint8_t>& lane, int alignment, size_t start) {
    if (lane.size() < start) {
        throw InvalidOperation("KV3 lane is shorter than its prefix.");
    }
    const size_t remaining = lane.size() - start;
    if (remaining == 0) {
        return;
    }
    AlignPad(dest, offset, alignment);
    dest.Add(lane.data() + start, remaining);
    offset += Fit(remaining, "lane");
}

void WriteHeaderPrefix(Buf& out, int version, const std::array<uint8_t, 16>& format) {
    out.Put(kMagicBase | static_cast<uint32_t>(version));
    out.Add(format.data(), format.size());
    out.Put(static_cast<uint32_t>(Compression::None));
    out.Put(static_cast<uint16_t>(0));
    out.Put(static_cast<uint16_t>(0));
}

std::vector<uint8_t> WriteVersion5(const KvDocument& document, Written& written) {
    Buf buffer1;
    int string_bytes = 0;
    for (const auto& text : written.strings) {
        buffer1.Add(text.data(), text.size());
        buffer1.Put(static_cast<uint8_t>(0));
        string_bytes += Fit(text.size() + 1, "string");
    }
    int offset = string_bytes;
    AlignPad(buffer1, offset, 4);
    buffer1.Put(Fit(written.strings.size(), "string count"));

    Buf buffer2;
    buffer2.Add(written.object_lengths.b.data(), written.object_lengths.b.size());
    offset = Fit(written.object_lengths.b.size(), "object table");
    WriteLane(buffer2, offset, written.bytes1.b, 1, 0);
    WriteLane(buffer2, offset, written.bytes2.b, 2, 0);
    WriteLane(buffer2, offset, written.bytes4.b, 4, 4);
    WriteLane(buffer2, offset, written.bytes8.b, 8, 0);
    buffer2.Add(written.types.b.data(), written.types.b.size());
    for (const int length : written.blob_lengths) {
        buffer2.Put(length);
    }
    buffer2.Put(kTrailer);

    const int objects = Fit(written.object_lengths.b.size() / 4, "object count");
    if (objects > std::numeric_limits<uint16_t>::max() || written.arrays > std::numeric_limits<uint16_t>::max()) {
        throw InvalidData("KV3 object or array count does not fit.");
    }
    if (written.bytes4.b.size() < 4 || written.bytes2.b.size() % 2 != 0 || written.bytes8.b.size() % 8 != 0
        || written.bytes4.b.size() % 4 != 0) {
        throw InvalidOperation("KV3 value lanes are misaligned.");
    }

    Buf out;
    WriteHeaderPrefix(out, 5, document.Format);
    out.Put(string_bytes);
    out.Put(1);
    out.Put(0);
    out.Put(Fit(written.types.b.size(), "type bytes"));
    out.Put(static_cast<uint16_t>(objects));
    out.Put(static_cast<uint16_t>(written.arrays));
    out.Put(Fit(buffer1.b.size() + buffer2.b.size(), "buffer"));
    out.Put(Fit(buffer1.b.size() + buffer2.b.size() + written.blobs.b.size(), "buffer"));
    out.Put(Fit(written.blob_lengths.size(), "blob count"));
    out.Put(Fit(written.blobs.b.size(), "blob bytes"));
    out.Put(0);
    out.Put(0);
    out.Put(Fit(buffer1.b.size(), "buffer"));
    out.Put(0);
    out.Put(Fit(buffer2.b.size(), "buffer"));
    out.Put(0);
    out.Put(Fit(written.bytes1.b.size(), "byte count"));
    out.Put(Fit(written.bytes2.b.size() / 2, "word count"));
    out.Put(Fit(written.bytes4.b.size() / 4 - 1, "dword count"));
    out.Put(Fit(written.bytes8.b.size() / 8, "qword count"));
    out.Put(0);
    out.Put(objects);
    out.Put(written.arrays);
    out.Put(0);
    out.Add(buffer1.b.data(), buffer1.b.size());
    out.Add(buffer2.b.data(), buffer2.b.size());
    out.Add(written.blobs.b.data(), written.blobs.b.size());
    if (!written.blob_lengths.empty()) {
        out.Put(kTrailer);
    }
    return std::move(out.b);
}

std::vector<uint8_t> WriteVersion4(const KvDocument& document, Written& written) {
    Buf buffer;
    int offset = 0;
    WriteLane(buffer, offset, written.bytes1.b, 1, 0);
    WriteLane(buffer, offset, written.bytes2.b, 2, 0);
    WriteLane(buffer, offset, written.bytes4.b, 4, 0);
    if (!written.bytes8.b.empty()) {
        WriteLane(buffer, offset, written.bytes8.b, 8, 0);
    } else {
        AlignPad(buffer, offset, 8);
    }
    const int strings_start = offset;
    for (const auto& text : written.strings) {
        buffer.Add(text.data(), text.size());
        buffer.Put(static_cast<uint8_t>(0));
        offset += Fit(text.size() + 1, "string");
    }
    buffer.Add(written.types.b.data(), written.types.b.size());
    offset += Fit(written.types.b.size(), "type bytes");
    const int type_span = offset - strings_start;
    if (buffer.b.size() != static_cast<size_t>(offset)) {
        throw InvalidOperation("KV3 v4 buffer offset drifted.");
    }
    if (written.blob_lengths.empty()) {
        buffer.Put(kTrailer);
    } else {
        for (const int length : written.blob_lengths) {
            buffer.Put(length);
        }
        buffer.Put(kTrailer);
    }

    Buf out;
    WriteHeaderPrefix(out, 4, document.Format);
    out.Put(Fit(written.bytes1.b.size(), "byte count"));
    out.Put(Fit(written.bytes4.b.size() / 4, "dword count"));
    out.Put(Fit(written.bytes8.b.size() / 8, "qword count"));
    out.Put(type_span);
    out.Put(static_cast<uint16_t>(0));
    out.Put(static_cast<uint16_t>(0));
    out.Put(Fit(buffer.b.size(), "buffer"));
    out.Put(Fit(buffer.b.size(), "buffer"));
    out.Put(Fit(written.blob_lengths.size(), "blob count"));
    out.Put(Fit(written.blobs.b.size(), "blob bytes"));
    out.Put(Fit(written.bytes2.b.size() / 2, "word count"));
    out.Put(0);
    out.Add(buffer.b.data(), buffer.b.size());
    if (!written.blob_lengths.empty()) {
        out.Add(written.blobs.b.data(), written.blobs.b.size());
        out.Put(kTrailer);
    }
    return std::move(out.b);
}

}  // namespace

bool IsBinaryKv3(uint32_t magic) {
    const uint32_t version = magic & 0xFFu;
    return magic == kMagic0 || ((magic & 0xFFFFFF00u) == kMagicBase && version >= 1 && version <= 5);
}

KvDocument ReadKv3(std::span<const uint8_t> bytes) {
    Lane file{bytes.data(), bytes.size(), 0};
    const Header header = ReadHeader(file);

    std::vector<uint8_t> buffer1_storage;
    std::vector<uint8_t> combined_zstd;
    if (header.compression == Compression::Zstd && header.version < 5) {
        const int output = header.unc1 + header.size_blobs;
        combined_zstd = DecompressZstd(file.TakeBytes(static_cast<size_t>(header.comp1)), output);
        buffer1_storage.assign(combined_zstd.begin(), combined_zstd.begin() + header.unc1);
    } else {
        const int compressed = header.version >= 5 ? header.comp1 : header.size_compressed;
        buffer1_storage = ReadCompressed(file, header.compression, compressed, header.unc1);
    }

    Context ctx;
    ctx.version = header.version;
    size_t offset = 0;
    if (header.version >= 5) {
        SplitLeadingLanes(buffer1_storage, 5, header.count1, header.count2, header.count4, header.count8, ctx.aux, offset);
        if (offset != buffer1_storage.size()) {
            throw InvalidData("KV3 buffer has trailing bytes.");
        }
        const int string_count = ctx.aux.b4.Take<int32_t>();
        ReadStrings(ctx.aux.b1, string_count, ctx.strings);

        auto buffer2_storage = ReadCompressed(file, header.compression, header.comp2, header.unc2);
        offset = Mul(header.objects2, 4);
        if (offset > buffer2_storage.size()) {
            throw InvalidData("KV3 object table is outside the buffer.");
        }
        ctx.object_lengths = Lane{buffer2_storage.data(), offset, 0};
        Lanes body;
        size_t body_offset = offset;
        const auto* base = buffer2_storage.data();
        const size_t size = buffer2_storage.size();
        body.b1 = TakeLane(base, size, body_offset, header.b2_count1, 1, 1);
        body.b2 = TakeLane(base, size, body_offset, header.b2_count2, 2, 2);
        body.b4 = TakeLane(base, size, body_offset, header.b2_count4, 4, 4);
        body.b8 = TakeLane(base, size, body_offset, header.b2_count8, 8, 8);
        ctx.buf = body;
        if (static_cast<size_t>(header.count_types) > size - body_offset) {
            throw InvalidData("KV3 types are outside the buffer.");
        }
        ctx.types = Lane{base + body_offset, static_cast<size_t>(header.count_types), 0};
        body_offset += static_cast<size_t>(header.count_types);

        std::vector<uint8_t> blob_storage;
        if (header.count_blocks == 0) {
            if (body_offset + 4 != size) {
                throw InvalidData("KV3 buffer trailer is missing.");
            }
            CheckTrailer(Lane{base + body_offset, 4, 0}.Take<uint32_t>());
        } else {
            Lane tail{base + body_offset, size - body_offset, 0};
            const auto lengths = tail.TakeBytes(Mul(header.count_blocks, 4));
            ctx.blob_lengths = Lane{lengths.data(), lengths.size(), 0};
            CheckTrailer(tail.Take<uint32_t>());
            if (header.compression == Compression::Lz4) {
                blob_storage = DecompressLz4Chain(tail, file, header.frame, header.size_blobs);
            } else if (header.compression == Compression::Zstd) {
                if (header.size_block_sizes != 0 || !tail.Done()) {
                    throw InvalidData("KV3 Zstd blob frame table is not supported.");
                }
                const int compressed = header.size_compressed - header.comp1 - header.comp2;
                if (compressed < 0) {
                    throw InvalidData("KV3 Zstd blob size is invalid.");
                }
                blob_storage = DecompressZstd(file.TakeBytes(static_cast<size_t>(compressed)), header.size_blobs);
            } else {
                if (!tail.Done()) {
                    throw InvalidData("KV3 blob frame table was not fully consumed.");
                }
                const auto raw = file.TakeBytes(static_cast<size_t>(header.size_blobs));
                blob_storage.assign(raw.begin(), raw.end());
            }
            if (!tail.Done()) {
                throw InvalidData("KV3 blob frame table was not fully consumed.");
            }
            ctx.blobs = Lane{blob_storage.data(), blob_storage.size(), 0};
            CheckTrailer(file.Take<uint32_t>());
        }

        const auto [root_type, root_flag] = ReadType(ctx);
        KvDocument document;
        document.Format = header.format;
        document.Version = 5;
        document.Root = ReadNode(ctx, root_type, root_flag, 0);
        Exhaust(ctx.types, "type bytes");
        Exhaust(ctx.object_lengths, "object table");
        Exhaust(ctx.blob_lengths, "blob lengths");
        Exhaust(ctx.blobs, "blobs");
        Exhaust(ctx.buf.b1, "byte lane");
        Exhaust(ctx.buf.b2, "word lane");
        Exhaust(ctx.buf.b4, "dword lane");
        Exhaust(ctx.buf.b8, "qword lane");
        Exhaust(ctx.aux.b1, "string bytes");
        Exhaust(ctx.aux.b2, "auxiliary words");
        Exhaust(ctx.aux.b4, "auxiliary dwords");
        Exhaust(ctx.aux.b8, "auxiliary qwords");
        if (!file.Done()) {
            throw InvalidData("KV3 data has trailing bytes.");
        }
        return document;
    }

    SplitLeadingLanes(buffer1_storage, header.version, header.count1, header.count2, header.count4, header.count8, ctx.buf, offset);
    const int string_count = ctx.buf.b4.Take<int32_t>();
    const size_t strings_start = offset;
    Lane string_bytes{buffer1_storage.data() + offset, buffer1_storage.size() - offset, 0};
    ReadStrings(string_bytes, string_count, ctx.strings);
    offset = strings_start + string_bytes.i;

    int types_length = 0;
    if (header.version == 1) {
        if (header.size_uncompressed < static_cast<int>(offset) + 4) {
            throw InvalidData("KV3 types are outside the buffer.");
        }
        types_length = header.size_uncompressed - static_cast<int>(offset) - 4;
    } else {
        const long long length = static_cast<long long>(header.count_types) - static_cast<long long>(offset) + static_cast<long long>(strings_start);
        if (length < 0 || length > std::numeric_limits<int>::max()) {
            throw InvalidData("KV3 type span is invalid.");
        }
        types_length = static_cast<int>(length);
    }
    if (static_cast<size_t>(types_length) > buffer1_storage.size() - offset) {
        throw InvalidData("KV3 types are outside the buffer.");
    }
    ctx.types = Lane{buffer1_storage.data() + offset, static_cast<size_t>(types_length), 0};
    offset += static_cast<size_t>(types_length);

    std::vector<uint8_t> legacy_blob_storage;
    if (header.compression == Compression::Zstd && header.version < 5 && header.count_blocks > 0) {
        legacy_blob_storage = std::move(combined_zstd);
    }
    if (header.count_blocks == 0) {
        if (offset + 4 != buffer1_storage.size()) {
            throw InvalidData("KV3 buffer trailer is missing.");
        }
        CheckTrailer(Lane{buffer1_storage.data() + offset, 4, 0}.Take<uint32_t>());
    } else {
        Lane tail{buffer1_storage.data() + offset, buffer1_storage.size() - offset, 0};
        const auto lengths = tail.TakeBytes(Mul(header.count_blocks, 4));
        ctx.blob_lengths = Lane{lengths.data(), lengths.size(), 0};
        CheckTrailer(tail.Take<uint32_t>());
        std::vector<uint8_t> blob_storage;
        if (header.compression == Compression::Lz4) {
            blob_storage = DecompressLz4Chain(tail, file, header.frame, header.size_blobs);
            ctx.blobs = Lane{blob_storage.data(), blob_storage.size(), 0};
            Exhaust(tail, "blob frame table");
            CheckTrailer(file.Take<uint32_t>());
            const auto [root_type, root_flag] = ReadType(ctx);
            KvDocument document;
            document.Format = header.format;
            document.Version = header.version == 4 ? 4 : 5;
            document.Root = ReadNode(ctx, root_type, root_flag, 0);
            // blob_storage must stay alive through ReadNode. It does, this block owns it.
            Exhaust(ctx.types, "type bytes");
            Exhaust(ctx.blob_lengths, "blob lengths");
            Exhaust(ctx.blobs, "blobs");
            Exhaust(ctx.buf.b1, "byte lane");
            Exhaust(ctx.buf.b2, "word lane");
            Exhaust(ctx.buf.b4, "dword lane");
            Exhaust(ctx.buf.b8, "qword lane");
            if (!file.Done()) {
                throw InvalidData("KV3 data has trailing bytes.");
            }
            return document;
        }
        if (header.compression == Compression::Zstd) {
            if (legacy_blob_storage.size() < static_cast<size_t>(header.unc1) + static_cast<size_t>(header.size_blobs)) {
                throw InvalidData("KV3 Zstd blob is outside the buffer.");
            }
            ctx.blobs = Lane{legacy_blob_storage.data() + header.unc1, static_cast<size_t>(header.size_blobs), 0};
        } else {
            const auto raw = file.TakeBytes(static_cast<size_t>(header.size_blobs));
            blob_storage.assign(raw.begin(), raw.end());
            ctx.blobs = Lane{blob_storage.data(), blob_storage.size(), 0};
        }
        Exhaust(tail, "blob frame table");
        CheckTrailer(file.Take<uint32_t>());
        const auto [root_type, root_flag] = ReadType(ctx);
        KvDocument document;
        document.Format = header.format;
        document.Version = header.version == 4 ? 4 : 5;
        document.Root = ReadNode(ctx, root_type, root_flag, 0);
        Exhaust(ctx.types, "type bytes");
        Exhaust(ctx.blob_lengths, "blob lengths");
        Exhaust(ctx.blobs, "blobs");
        Exhaust(ctx.buf.b1, "byte lane");
        Exhaust(ctx.buf.b2, "word lane");
        Exhaust(ctx.buf.b4, "dword lane");
        Exhaust(ctx.buf.b8, "qword lane");
        if (!file.Done()) {
            throw InvalidData("KV3 data has trailing bytes.");
        }
        return document;
    }

    const auto [root_type, root_flag] = ReadType(ctx);
    KvDocument document;
    document.Format = header.format;
    document.Version = header.version == 4 ? 4 : 5;
    document.Root = ReadNode(ctx, root_type, root_flag, 0);
    Exhaust(ctx.types, "type bytes");
    Exhaust(ctx.blob_lengths, "blob lengths");
    Exhaust(ctx.blobs, "blobs");
    Exhaust(ctx.buf.b1, "byte lane");
    Exhaust(ctx.buf.b2, "word lane");
    Exhaust(ctx.buf.b4, "dword lane");
    Exhaust(ctx.buf.b8, "qword lane");
    if (!file.Done()) {
        throw InvalidData("KV3 data has trailing bytes.");
    }
    return document;
}

std::vector<uint8_t> WriteKv3(const KvDocument& document) {
    if (document.Version != 4 && document.Version != 5) {
        throw InvalidOperation("Only KV3 versions 4 and 5 can be written.");
    }
    Written written;
    written.version5 = document.Version == 5;
    written.bytes4.Put(static_cast<int32_t>(0));
    WriteValue(written, document.Root);
    if (written.bytes4.b.size() < 4) {
        throw InvalidOperation("KV3 string count is missing.");
    }
    const int32_t string_count = Fit(written.strings.size(), "string count");
    std::memcpy(written.bytes4.b.data(), &string_count, sizeof(string_count));
    if (document.Version == 5) {
        return WriteVersion5(document, written);
    }
    return WriteVersion4(document, written);
}

namespace {

void Require(bool ok, const char* message) {
    if (!ok) {
        throw InvalidData(message);
    }
}

}  // namespace

KvValue KvValue::Null() { return {}; }

KvValue KvValue::Bool(bool value) {
    KvValue out;
    out.Kind = KvKind::Bool;
    out.bool_ = value;
    return out;
}

KvValue KvValue::Int16(int16_t value) {
    KvValue out;
    out.Kind = KvKind::Int16;
    out.i64_ = value;
    return out;
}

KvValue KvValue::UInt16(uint16_t value) {
    KvValue out;
    out.Kind = KvKind::UInt16;
    out.u64_ = value;
    return out;
}

KvValue KvValue::Int32(int32_t value) {
    KvValue out;
    out.Kind = KvKind::Int32;
    out.i64_ = value;
    return out;
}

KvValue KvValue::UInt32(uint32_t value) {
    KvValue out;
    out.Kind = KvKind::UInt32;
    out.u64_ = value;
    return out;
}

KvValue KvValue::Int64(int64_t value) {
    KvValue out;
    out.Kind = KvKind::Int64;
    out.i64_ = value;
    return out;
}

KvValue KvValue::UInt64(uint64_t value) {
    KvValue out;
    out.Kind = KvKind::UInt64;
    out.u64_ = value;
    return out;
}

KvValue KvValue::Float(float value) {
    KvValue out;
    out.Kind = KvKind::Float;
    out.f32_ = value;
    return out;
}

KvValue KvValue::Double(double value) {
    KvValue out;
    out.Kind = KvKind::Double;
    out.f64_ = value;
    return out;
}

KvValue KvValue::String(std::string value) {
    KvValue out;
    out.Kind = KvKind::String;
    out.text_ = std::move(value);
    return out;
}

KvValue KvValue::Blob(std::vector<uint8_t> value) {
    KvValue out;
    out.Kind = KvKind::Blob;
    out.blob_ = std::move(value);
    return out;
}

KvValue KvValue::Array(std::vector<KvValue> values) {
    KvValue out;
    out.Kind = KvKind::Array;
    out.items_ = std::move(values);
    return out;
}

KvValue KvValue::Object(std::vector<std::pair<std::string, KvValue>> fields) {
    KvValue out;
    out.Kind = KvKind::Object;
    out.fields_ = std::move(fields);
    return out;
}

bool KvValue::Bool() const {
    Require(Kind == KvKind::Bool, "KV3 value is not a boolean.");
    return bool_;
}

int16_t KvValue::Int16() const {
    Require(Kind == KvKind::Int16, "KV3 value is not an int16.");
    return static_cast<int16_t>(i64_);
}

uint16_t KvValue::UInt16() const {
    Require(Kind == KvKind::UInt16, "KV3 value is not a uint16.");
    return static_cast<uint16_t>(u64_);
}

int32_t KvValue::Int32() const {
    Require(Kind == KvKind::Int32, "KV3 value is not an int32.");
    return static_cast<int32_t>(i64_);
}

uint32_t KvValue::UInt32() const {
    Require(Kind == KvKind::UInt32, "KV3 value is not a uint32.");
    return static_cast<uint32_t>(u64_);
}

int64_t KvValue::Int64() const {
    Require(Kind == KvKind::Int64, "KV3 value is not an int64.");
    return i64_;
}

uint64_t KvValue::UInt64() const {
    Require(Kind == KvKind::UInt64, "KV3 value is not a uint64.");
    return u64_;
}

float KvValue::Float() const {
    Require(Kind == KvKind::Float, "KV3 value is not a float.");
    return f32_;
}

double KvValue::Double() const {
    Require(Kind == KvKind::Double, "KV3 value is not a double.");
    return f64_;
}

const std::string& KvValue::String() const {
    Require(Kind == KvKind::String, "KV3 value is not a string.");
    return text_;
}

const std::vector<uint8_t>& KvValue::Blob() const {
    Require(Kind == KvKind::Blob, "KV3 value is not a blob.");
    return blob_;
}

int32_t KvValue::ToInt32() const {
    int64_t value = 0;
    switch (Kind) {
    case KvKind::Int16:
    case KvKind::Int32:
    case KvKind::Int64:
        value = i64_;
        break;
    case KvKind::UInt16:
    case KvKind::UInt32:
        value = static_cast<int64_t>(u64_);
        break;
    case KvKind::UInt64:
        if (u64_ > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw InvalidData("KV3 integer does not fit.");
        }
        value = static_cast<int64_t>(u64_);
        break;
    default:
        throw InvalidData("KV3 value is not an integer.");
    }
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) {
        throw InvalidData("KV3 integer does not fit.");
    }
    return static_cast<int32_t>(value);
}

double KvValue::ToDouble() const {
    switch (Kind) {
    case KvKind::Int16:
    case KvKind::Int32:
    case KvKind::Int64:
        return static_cast<double>(i64_);
    case KvKind::UInt16:
    case KvKind::UInt32:
    case KvKind::UInt64:
        return static_cast<double>(u64_);
    case KvKind::Float:
        return f32_;
    case KvKind::Double:
        return f64_;
    default:
        throw InvalidData("KV3 value is not a number.");
    }
}

size_t KvValue::Count() const {
    if (Kind == KvKind::Array) {
        return items_.size();
    }
    if (Kind == KvKind::Object) {
        return fields_.size();
    }
    return 0;
}

KvValue& KvValue::At(size_t index) {
    Require(Kind == KvKind::Array, "KV3 value is not an array.");
    Require(index < items_.size(), "KV3 array index is outside the array.");
    return items_[index];
}

const KvValue& KvValue::At(size_t index) const {
    Require(Kind == KvKind::Array, "KV3 value is not an array.");
    Require(index < items_.size(), "KV3 array index is outside the array.");
    return items_[index];
}

KvValue* KvValue::Find(std::string_view key) {
    Require(Kind == KvKind::Object, "KV3 value is not an object.");
    for (auto& field : fields_) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

const KvValue* KvValue::Find(std::string_view key) const {
    Require(Kind == KvKind::Object, "KV3 value is not an object.");
    for (const auto& field : fields_) {
        if (field.first == key) {
            return &field.second;
        }
    }
    return nullptr;
}

void KvValue::Set(std::string key, KvValue value) {
    Require(Kind == KvKind::Object, "KV3 field update requires an object.");
    for (auto& field : fields_) {
        if (field.first == key) {
            field.second = std::move(value);
            return;
        }
    }
    fields_.emplace_back(std::move(key), std::move(value));
}

void KvValue::Add(KvValue value) {
    Require(Kind == KvKind::Array, "KV3 append requires an array.");
    items_.push_back(std::move(value));
}

std::vector<KvValue>& KvValue::Items() {
    Require(Kind == KvKind::Array, "KV3 value is not an array.");
    return items_;
}

const std::vector<KvValue>& KvValue::Items() const {
    Require(Kind == KvKind::Array, "KV3 value is not an array.");
    return items_;
}

std::vector<std::pair<std::string, KvValue>>& KvValue::Fields() {
    Require(Kind == KvKind::Object, "KV3 value is not an object.");
    return fields_;
}

const std::vector<std::pair<std::string, KvValue>>& KvValue::Fields() const {
    Require(Kind == KvKind::Object, "KV3 value is not an object.");
    return fields_;
}

}  // namespace skeleton_repair
