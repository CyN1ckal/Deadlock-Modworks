#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

// Replaces the DATA block of a compiled Source 2 resource and patches the block table.
std::vector<uint8_t> ReplaceDataBlock(std::span<const uint8_t> file, std::span<const uint8_t> new_data);

// The DATA bytes inside a version 12 resource. The span points into `file`.
std::span<const uint8_t> FindDataBlock(std::span<const uint8_t> file);

// Names from the first RERL block. A missing or empty block returns no names.
// Layout pinned to ValveResourceFormat 20.0.6980 ResourceExtRefList.
std::vector<std::string> ExternalReferenceNames(std::span<const uint8_t> file);

}  // namespace skeleton_repair
