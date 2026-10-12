#pragma once
#include "model_library.hpp"

namespace skeleton_repair {
// Owns embedded VPK copies and an automatically saved collection manifest.
class LocalModStore {
public:
    explicit LocalModStore(std::filesystem::path root);
    static std::filesystem::path DefaultRoot();
    const std::filesystem::path& Root() const { return root_; }
    std::filesystem::path Import(const std::filesystem::path& source, CancellationToken cancellation = {}) const;
    ModelLibrary Load() const;
    void Save(const ModelLibrary& library) const;
private:
    std::filesystem::path root_;
};
}
