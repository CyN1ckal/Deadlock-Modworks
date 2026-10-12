#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skeleton_repair {

// KV3 flag byte written by ValveResourceFormat 20. Values above EntityName are rejected.
enum class KvFlag : uint8_t {
    None = 0,
    Resource = 1,
    ResourceName = 2,
    Panorama = 3,
    SoundEvent = 4,
    SubClass = 5,
    EntityName = 6,
};

enum class KvKind : uint8_t {
    Null,
    Bool,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    Blob,
    Array,
    Object,
};

// One KeyValues3 value. Object keys keep their file order, including duplicates.
class KvValue {
public:
    KvKind Kind = KvKind::Null;
    KvFlag Flag = KvFlag::None;

    static KvValue Null();
    static KvValue Bool(bool value);
    static KvValue Int16(int16_t value);
    static KvValue UInt16(uint16_t value);
    static KvValue Int32(int32_t value);
    static KvValue UInt32(uint32_t value);
    static KvValue Int64(int64_t value);
    static KvValue UInt64(uint64_t value);
    static KvValue Float(float value);
    static KvValue Double(double value);
    static KvValue String(std::string value);
    static KvValue Blob(std::vector<uint8_t> value);
    static KvValue Array(std::vector<KvValue> values);
    static KvValue Object(std::vector<std::pair<std::string, KvValue>> fields);

    bool Bool() const;
    int16_t Int16() const;
    uint16_t UInt16() const;
    int32_t Int32() const;
    uint32_t UInt32() const;
    int64_t Int64() const;
    uint64_t UInt64() const;
    float Float() const;
    double Double() const;
    const std::string& String() const;
    const std::vector<uint8_t>& Blob() const;

    // Checked narrowing used by the skeleton repair. Floats are not accepted as ints.
    int32_t ToInt32() const;
    double ToDouble() const;

    bool IsArray() const { return Kind == KvKind::Array; }
    bool IsObject() const { return Kind == KvKind::Object; }
    size_t Count() const;

    KvValue& At(size_t index);
    const KvValue& At(size_t index) const;
    KvValue* Find(std::string_view key);
    const KvValue* Find(std::string_view key) const;
    bool Contains(std::string_view key) const { return Find(key) != nullptr; }

    // Replaces the first field of this name, or appends one.
    void Set(std::string key, KvValue value);
    void Add(KvValue value);

    std::vector<KvValue>& Items();
    const std::vector<KvValue>& Items() const;
    std::vector<std::pair<std::string, KvValue>>& Fields();
    const std::vector<std::pair<std::string, KvValue>>& Fields() const;

private:
    bool bool_ = false;
    int64_t i64_ = 0;
    uint64_t u64_ = 0;
    float f32_ = 0;
    double f64_ = 0;
    std::string text_;
    std::vector<uint8_t> blob_;
    std::vector<KvValue> items_;
    std::vector<std::pair<std::string, KvValue>> fields_;
};

struct KvDocument {
    // generic encoding 7412167c-06e9-4698-aff2-e63eb59037e7 in Guid.ToByteArray order.
    static constexpr std::array<uint8_t, 16> GenericFormat = {
        0x7c, 0x16, 0x12, 0x74, 0xe9, 0x06, 0x98, 0x46,
        0xaf, 0xf2, 0xe6, 0x3e, 0xb5, 0x90, 0x37, 0xe7,
    };

    std::array<uint8_t, 16> Format = GenericFormat;
    // Version 4 is written as 4. Every other supported version is written as 5.
    int Version = 5;
    KvValue Root = KvValue::Object({});
};

bool IsBinaryKv3(uint32_t magic);

// Reads KV3 versions 1 through 5, uncompressed, LZ4, or Zstd.
// Version 4 stays 4. Versions 1, 2, 3, and 5 are recorded as 5, which is how they are written back.
KvDocument ReadKv3(std::span<const uint8_t> bytes);

// Writes uncompressed KV3 version 4 or 5. Other versions are rejected.
std::vector<uint8_t> WriteKv3(const KvDocument& document);

}  // namespace skeleton_repair
