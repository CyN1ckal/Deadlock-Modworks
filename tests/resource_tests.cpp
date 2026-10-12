#include "error.hpp"
#include "kv3.hpp"
#include "resource.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {

std::vector<uint8_t> Load(const char* name) {
    const auto path = std::filesystem::path(SR_SOURCE_DIR) / "tests" / "data" / name;
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << path.string();
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteI32(std::vector<uint8_t>& bytes, size_t offset, int32_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

// One DATA block, optionally followed by a 4-byte PHYS block, matching the C# fixture layout.
std::vector<uint8_t> Resource(const std::vector<uint8_t>& data, bool trailing_block) {
    const int count = trailing_block ? 2 : 1;
    const int start = 16 + count * 12;
    std::vector<uint8_t> file(static_cast<size_t>(start) + data.size() + (trailing_block ? 4u : 0u));
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, count);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, static_cast<uint32_t>(start - 20));
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    if (trailing_block) {
        file[28] = 'P';
        file[29] = 'H';
        file[30] = 'Y';
        file[31] = 'S';
        WriteU32(file, 32, static_cast<uint32_t>(start + data.size() - 32));
        WriteI32(file, 36, 4);
        file[start + data.size()] = 1;
        file[start + data.size() + 1] = 2;
        file[start + data.size() + 2] = 3;
        file[start + data.size() + 3] = 4;
    }
    return file;
}

}  // namespace

TEST(Resource, ReplacesDataAndSlidesATrailingBlock) {
    const auto original = Load("empty-object.kv3");
    const auto replacement = Load("scalars.kv3");
    const auto file = Resource(original, true);
    const auto patched = skeleton_repair::ReplaceDataBlock(file, replacement);

    EXPECT_EQ(patched.size(), file.size() - original.size() + replacement.size());
    EXPECT_EQ(ReadU32(patched, 0), patched.size());
    EXPECT_EQ(ReadU32(patched, 24), replacement.size());
    EXPECT_EQ(std::vector<uint8_t>(patched.begin() + 40, patched.begin() + 40 + static_cast<std::ptrdiff_t>(replacement.size())), replacement);
    EXPECT_EQ(patched[patched.size() - 4], 1);
    EXPECT_EQ(patched[patched.size() - 1], 4);

    const uint32_t old_rel = ReadU32(file, 32);
    const uint32_t new_rel = ReadU32(patched, 32);
    EXPECT_EQ(new_rel, old_rel + static_cast<uint32_t>(replacement.size() - original.size()));
    const auto data = skeleton_repair::ReadKv3(std::span<const uint8_t>(patched.data() + 40, replacement.size()));
    EXPECT_EQ(data.Root.Find("text")->String(), "hero");
}

TEST(Resource, LeavesABlockBeforeDataWhereItIs) {
    const auto data = Load("empty-object.kv3");
    std::vector<uint8_t> file(44 + data.size());
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, 2);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, 24);
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    file[28] = 'R';
    file[29] = 'E';
    file[30] = 'D';
    file[31] = 'I';
    WriteU32(file, 32, 8);
    WriteI32(file, 36, 4);
    file[40] = 0x11;
    file[41] = 0x12;
    file[42] = 0x13;
    file[43] = 0x14;
    std::memcpy(file.data() + 44, data.data(), data.size());

    const auto patched = skeleton_repair::ReplaceDataBlock(file, Load("scalars.kv3"));
    EXPECT_EQ(ReadU32(patched, 32), 8u);
    EXPECT_EQ(patched[40], 0x11);
    EXPECT_EQ(patched[43], 0x14);
    EXPECT_EQ(ReadU32(patched, 24), static_cast<uint32_t>(Load("scalars.kv3").size()));
}

TEST(Resource, RejectsBrokenTablesAndPayloads) {
    const auto kv = Load("empty-object.kv3");
    auto file = Resource(kv, false);
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(std::span<const uint8_t>(file.data(), 15), kv), skeleton_repair::InvalidData);
    WriteU16(file, 4, 11);
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);

    file = Resource(kv, false);
    const std::vector<uint8_t> foreign{0x00, 0x33, 0x56, 0x4B};
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, foreign), skeleton_repair::InvalidData);

    file = Resource(kv, false);
    WriteI32(file, 12, std::numeric_limits<int32_t>::max());
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);

    file = Resource(kv, true);
    file[28] = 'D';
    file[29] = 'A';
    file[30] = 'T';
    file[31] = 'A';
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);

    file = Resource(kv, false);
    file[16] = 'R';
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);

    file = Resource(kv, true);
    WriteI32(file, 36, -1);
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);

    file = Resource(kv, true);
    WriteU32(file, 32, 0);
    EXPECT_THROW(skeleton_repair::ReplaceDataBlock(file, kv), skeleton_repair::InvalidData);
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 4);
    WriteU32(bytes, offset, value);
}

void AppendI32(std::vector<uint8_t>& bytes, int32_t value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + 4);
    WriteI32(bytes, offset, value);
}

void AppendU64(std::vector<uint8_t>& bytes, uint64_t value) {
    AppendU32(bytes, static_cast<uint32_t>(value));
    AppendU32(bytes, static_cast<uint32_t>(value >> 32));
}

std::vector<uint8_t> RerlBlock(const std::vector<std::string>& names) {
    std::vector<uint8_t> block;
    const auto count = static_cast<uint32_t>(names.size());
    AppendU32(block, 8);
    AppendU32(block, count);
    std::string strings;
    std::vector<int32_t> offsets;
    offsets.reserve(names.size());
    size_t cursor = 8 + static_cast<size_t>(count) * 16;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t name_field = 16 + static_cast<size_t>(i) * 16;
        if (names[i].empty()) {
            offsets.push_back(0);
            continue;
        }
        offsets.push_back(static_cast<int32_t>(cursor - name_field));
        strings += names[i];
        strings.push_back('\0');
        cursor += names[i].size() + 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        AppendU64(block, 0);
        AppendI32(block, offsets[i]);
        AppendI32(block, 0);
    }
    block.insert(block.end(), strings.begin(), strings.end());
    return block;
}

std::vector<uint8_t> ResourceWithRerl(const std::vector<uint8_t>& data, const std::vector<uint8_t>& rerl) {
    const size_t start = 40;
    std::vector<uint8_t> file(start + data.size() + rerl.size());
    WriteU32(file, 0, static_cast<uint32_t>(file.size()));
    WriteU16(file, 4, 12);
    WriteU16(file, 6, 1);
    WriteU32(file, 8, 8);
    WriteI32(file, 12, 2);
    file[16] = 'D';
    file[17] = 'A';
    file[18] = 'T';
    file[19] = 'A';
    WriteU32(file, 20, 20);
    WriteI32(file, 24, static_cast<int32_t>(data.size()));
    file[28] = 'R';
    file[29] = 'E';
    file[30] = 'R';
    file[31] = 'L';
    WriteU32(file, 32, static_cast<uint32_t>(8 + data.size()));
    WriteI32(file, 36, static_cast<int32_t>(rerl.size()));
    std::memcpy(file.data() + start, data.data(), data.size());
    if (!rerl.empty()) {
        std::memcpy(file.data() + start + data.size(), rerl.data(), rerl.size());
    }
    return file;
}

TEST(Resource, ReadsTheFirstExternalReferenceList) {
    const auto data = Load("empty-object.kv3");
    EXPECT_TRUE(skeleton_repair::ExternalReferenceNames(Resource(data, false)).empty());

    auto empty_list = Resource(data, false);
    empty_list.resize(empty_list.size() + 12);
    WriteU32(empty_list, 0, static_cast<uint32_t>(empty_list.size()));
    WriteI32(empty_list, 12, 2);
    const size_t entry = 28;
    empty_list[entry] = 'R';
    empty_list[entry + 1] = 'E';
    empty_list[entry + 2] = 'R';
    empty_list[entry + 3] = 'L';
    WriteU32(empty_list, entry + 4, 8);
    WriteI32(empty_list, entry + 8, 0);
    EXPECT_TRUE(skeleton_repair::ExternalReferenceNames(empty_list).empty());
    EXPECT_TRUE(skeleton_repair::ExternalReferenceNames(ResourceWithRerl(data, RerlBlock({}))).empty());

    const auto names = skeleton_repair::ExternalReferenceNames(ResourceWithRerl(data, RerlBlock({
        "models/heroes/test/missing.vnmskel",
        "models/heroes/test/old.vanmgrph",
    })));
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "models/heroes/test/missing.vnmskel");
    EXPECT_EQ(names[1], "models/heroes/test/old.vanmgrph");

    const auto blank = skeleton_repair::ExternalReferenceNames(ResourceWithRerl(data, RerlBlock({""})));
    ASSERT_EQ(blank.size(), 1u);
    EXPECT_TRUE(blank[0].empty());

    auto truncated = ResourceWithRerl(data, RerlBlock({"models/heroes/test/missing.vnmskel"}));
    WriteI32(truncated, 36, 4);
    EXPECT_THROW(skeleton_repair::ExternalReferenceNames(truncated), skeleton_repair::InvalidData);
}
