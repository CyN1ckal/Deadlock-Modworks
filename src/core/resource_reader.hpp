#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// Returns a compiled resource's bytes by path ("..._c"), or nothing when it is missing.
using ResourceReader = std::function<std::optional<std::vector<uint8_t>>(const std::string& path)>;

}  // namespace skeleton_repair
