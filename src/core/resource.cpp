#include "resource.hpp"

#include "error.hpp"
#include "kv3.hpp"

#include <cstring>
#include <limits>
#include <string>

namespace skeleton_repair {
namespace {

template <typename T>
T Read(std::span<const uint8_t> bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(std::vector<uint8_t>& bytes, size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

struct LocatedData {
    uint32_t file_size = 0;
    int entry = 0;
    int abs = 0;
    int size = 0;
    int table_start = 0;
    int block_count = 0;
};

LocatedData LocateDataBlock(std::span<const uint8_t> file) {
    const auto file_size = Read<uint32_t>(file, 0);
    const auto block_offset = Read<uint32_t>(file, 8);
    const auto block_count = Read<int32_t>(file, 12);
    if (block_count <= 0 || block_offset < 8) {
        throw InvalidData("Resource block table is invalid.");
    }

    const int64_t table_start_long = 8LL + block_offset;
    const int64_t table_bytes_long = static_cast<int64_t>(block_count) * 12;
    if (table_start_long > static_cast<int64_t>(file.size()) || table_bytes_long > static_cast<int64_t>(file.size()) - table_start_long) {
        throw InvalidData("Resource block table is outside the file.");
    }
    const int table_start = static_cast<int>(table_start_long);
    const int table_bytes = static_cast<int>(table_bytes_long);
    if (table_start < 16 || static_cast<size_t>(table_start) > file.size() || static_cast<size_t>(table_bytes) > file.size() - static_cast<size_t>(table_start)) {
        throw InvalidData("Resource block table is outside the file.");
    }

    int data_entry = -1;
    int data_abs = 0;
    int data_size = 0;
    for (int i = 0; i < block_count; ++i) {
        const int pos = table_start + i * 12;
        if (file[pos] == 'D' && file[pos + 1] == 'A' && file[pos + 2] == 'T' && file[pos + 3] == 'A') {
            if (data_entry >= 0) {
                throw InvalidData("Resource has more than one DATA block.");
            }
            const auto rel = Read<uint32_t>(file, static_cast<size_t>(pos + 4));
            const auto size = Read<int32_t>(file, static_cast<size_t>(pos + 8));
            const int64_t abs = static_cast<int64_t>(pos) + 4 + rel;
            if (size < 0 || abs > std::numeric_limits<int>::max()) {
                throw InvalidData("DATA block location is invalid.");
            }
            data_entry = pos;
            data_abs = static_cast<int>(abs);
            data_size = size;
        }
    }

    if (data_entry < 0) {
        throw InvalidData("Resource has no DATA block.");
    }
    if (static_cast<size_t>(data_abs) > file.size() || static_cast<size_t>(data_size) > file.size() - static_cast<size_t>(data_abs)) {
        throw InvalidData("DATA block is outside the file.");
    }

    const int table_end = table_start + table_bytes;
    if (data_abs < table_end) {
        throw InvalidData("DATA block starts before the end of the block table.");
    }

    const int data_end = data_abs + data_size;
    for (int i = 0; i < block_count; ++i) {
        const int pos = table_start + i * 12;
        if (pos == data_entry) {
            continue;
        }
        const auto rel = Read<uint32_t>(file, static_cast<size_t>(pos + 4));
        const auto size = Read<int32_t>(file, static_cast<size_t>(pos + 8));
        if (size < 0) {
            throw InvalidData("A block size is negative.");
        }
        const int64_t abs = static_cast<int64_t>(pos) + 4 + rel;
        const int64_t end = abs + size;
        if (abs < table_end || end > static_cast<int64_t>(file.size())) {
            throw InvalidData("A block is outside the resource data area.");
        }
        if (abs < data_end && end > data_abs) {
            throw InvalidData("A block overlaps the DATA block.");
        }
    }

    return LocatedData{file_size, data_entry, data_abs, data_size, table_start, block_count};
}

}  // namespace

std::span<const uint8_t> FindDataBlock(std::span<const uint8_t> file) {
    if (file.size() < 16) {
        throw InvalidData("Resource is too small.");
    }
    if (Read<uint16_t>(file, 4) != 12) {
        throw InvalidData("Resource header is not version 12.");
    }
    const LocatedData located = LocateDataBlock(file);
    return file.subspan(static_cast<size_t>(located.abs), static_cast<size_t>(located.size));
}

std::vector<uint8_t> ReplaceDataBlock(std::span<const uint8_t> file, std::span<const uint8_t> new_data) {
    if (file.size() < 16) {
        throw InvalidData("Resource is too small.");
    }
    if (Read<uint16_t>(file, 4) != 12) {
        throw InvalidData("Resource header is not version 12.");
    }
    if (new_data.size() < 4 || !IsBinaryKv3(Read<uint32_t>(new_data, 0))) {
        throw InvalidData("Replacement DATA is not KV3.");
    }

    const LocatedData located = LocateDataBlock(file);
    const uint32_t file_size = located.file_size;
    const int data_entry = located.entry;
    const int data_abs = located.abs;
    const int data_size = located.size;
    const int table_start = located.table_start;
    const int block_count = located.block_count;
    const int data_end = data_abs + data_size;

    const int64_t delta = static_cast<int64_t>(new_data.size()) - data_size;
    const int64_t new_length = static_cast<int64_t>(file.size()) + delta;
    if (new_length < 0 || new_length > std::numeric_limits<int>::max()) {
        throw InvalidData("Replacing DATA made the resource an invalid size.");
    }

    std::vector<uint8_t> result(static_cast<size_t>(new_length));
    std::memcpy(result.data(), file.data(), static_cast<size_t>(data_abs));
    std::memcpy(result.data() + data_abs, new_data.data(), new_data.size());
    std::memcpy(result.data() + data_abs + new_data.size(), file.data() + data_end, file.size() - static_cast<size_t>(data_end));

    const int64_t new_file_size = static_cast<int64_t>(file_size) + delta;
    if (new_file_size < 0 || new_file_size > std::numeric_limits<uint32_t>::max()) {
        throw InvalidData("Resource file size does not fit.");
    }
    Write<uint32_t>(result, 0, static_cast<uint32_t>(new_file_size));
    Write<uint32_t>(result, static_cast<size_t>(data_entry + 8), static_cast<uint32_t>(new_data.size()));

    for (int i = 0; i < block_count; ++i) {
        const int pos = table_start + i * 12;
        if (pos == data_entry) {
            continue;
        }
        const auto rel = Read<uint32_t>(file, static_cast<size_t>(pos + 4));
        const int64_t abs = static_cast<int64_t>(pos) + 4 + rel;
        if (abs < data_end) {
            continue;
        }
        const int64_t new_rel = static_cast<int64_t>(rel) + delta;
        if (new_rel < 0 || new_rel > std::numeric_limits<uint32_t>::max()) {
            throw InvalidData("A block offset does not fit after the DATA replacement.");
        }
        Write<uint32_t>(result, static_cast<size_t>(pos + 4), static_cast<uint32_t>(new_rel));
    }

    return result;
}

}  // namespace skeleton_repair
