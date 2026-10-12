#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace skeleton_repair {

std::array<uint8_t, 32> Sha256(std::span<const uint8_t> data);
std::array<uint8_t, 32> Sha256File(const std::filesystem::path& path);
std::string ToLowerHex(std::span<const uint8_t> bytes);

}  // namespace skeleton_repair
