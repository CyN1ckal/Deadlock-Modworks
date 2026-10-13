#pragma once

#include <filesystem>
#include <string>

namespace skeleton_repair {

// One model chosen for the pack: the archive it comes from and its path inside that archive.
struct LibraryModel {
    std::filesystem::path Archive;
    std::string Model;
    bool operator==(const LibraryModel&) const = default;
};

}  // namespace skeleton_repair
