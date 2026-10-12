#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace skeleton_repair {

// Replaces the DATA block of a compiled Source 2 resource and patches the block table.
std::vector<uint8_t> ReplaceDataBlock(std::span<const uint8_t> file, std::span<const uint8_t> new_data);

}  // namespace skeleton_repair
