#include "character_view.hpp"

#include "strings.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace skeleton_repair {
namespace {

std::string AsciiLower(std::string text) {
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::string ArchiveKey(const std::filesystem::path& path) {
    return AsciiLower(Narrow(path.wstring()));
}

std::string Identity(const LibraryModel& model) {
    return ArchiveKey(model.Archive) + "|" + AsciiLower(model.Model);
}

std::vector<std::string_view> PathParts(std::string_view path) {
    std::vector<std::string_view> parts;
    for (size_t start = 0; start <= path.size();) {
        size_t slash = path.size();
        for (size_t index = start; index < path.size(); ++index) {
            if (path[index] == '/' || path[index] == '\\') {
                slash = index;
                break;
            }
        }
        parts.emplace_back(path.substr(start, slash - start));
        if (slash == path.size()) break;
        start = slash + 1;
    }
    return parts;
}

}  // namespace

std::optional<std::string> CharacterOfModel(std::string_view model_path) {
    const std::vector<std::string_view> parts = PathParts(model_path);
    if (parts.size() < 4 || !EqualsIgnoreCase(parts[0], "models") || !StartsWithIgnoreCase(parts[1], "heroes")) {
        return std::nullopt;
    }
    if (parts[2].empty()) return std::nullopt;
    return std::string(parts[2]);
}

std::string ModelFileTitle(std::string_view model_path) {
    const size_t slash = model_path.find_last_of("/\\");
    std::string title(slash == std::string_view::npos ? model_path : model_path.substr(slash + 1));
    constexpr std::string_view kExtension = ".vmdl_c";
    if (title.size() >= kExtension.size() && EqualsIgnoreCase(std::string_view(title).substr(title.size() - kExtension.size()), kExtension)) {
        title.resize(title.size() - kExtension.size());
    }
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}

std::string SlotCaption(const LibraryModel& model) {
    const std::string file = Narrow(model.Archive.filename().wstring());
    const std::string title = ModelFileTitle(model.Model);
    if (file.empty()) return title;
    if (title.empty()) return file;
    return file + " - " + title;
}

ViewportGrid GridForViewportCount(int count) {
    if (count < 1) count = 1;
    if (count > 4) count = 4;
    if (count == 1) return {1, 1};
    if (count == 2) return {2, 1};
    if (count == 3) return {3, 1};
    return {2, 2};
}

std::vector<CharacterOption> CharactersIn(const std::vector<LibraryModel>& models) {
    std::vector<CharacterOption> options;
    for (const LibraryModel& model : models) {
        const std::optional<std::string> name = CharacterOfModel(model.Model);
        if (!name || name->empty()) continue;
        const auto found = std::find_if(options.begin(), options.end(), [&](const CharacterOption& option) {
            return EqualsIgnoreCase(option.Name, *name);
        });
        if (found == options.end()) options.push_back({*name, 1});
        else ++found->Count;
    }
    std::sort(options.begin(), options.end(), [](const CharacterOption& left, const CharacterOption& right) {
        return CompareIgnoreCase(left.Name, right.Name) < 0;
    });
    return options;
}

std::vector<size_t> CharacterSlots(const std::vector<LibraryModel>& models, size_t chosen, int limit) {
    if (limit < 1 || chosen >= models.size()) return {};
    std::vector<size_t> slots{chosen};
    const std::optional<std::string> character = CharacterOfModel(models[chosen].Model);
    if (!character) return slots;

    const std::string chosen_archive = ArchiveKey(models[chosen].Archive);
    const std::string chosen_identity = Identity(models[chosen]);
    std::map<std::string, std::vector<size_t>> by_archive;
    for (size_t index = 0; index < models.size(); ++index) {
        if (index == chosen) continue;
        const std::optional<std::string> name = CharacterOfModel(models[index].Model);
        if (!name || !EqualsIgnoreCase(*name, *character)) continue;
        if (Identity(models[index]) == chosen_identity) continue;
        by_archive[ArchiveKey(models[index].Archive)].push_back(index);
    }

    auto pick = [&](const std::vector<size_t>& group) {
        size_t best = group.front();
        bool best_same = EqualsIgnoreCase(models[best].Model, models[chosen].Model);
        for (size_t index : group) {
            const bool same = EqualsIgnoreCase(models[index].Model, models[chosen].Model);
            if (same != best_same) {
                if (same) {
                    best = index;
                    best_same = true;
                }
                continue;
            }
            if (CompareIgnoreCase(models[index].Model, models[best].Model) < 0) best = index;
        }
        return best;
    };

    std::vector<uint8_t> used(models.size(), 0);
    used[chosen] = 1;
    for (const auto& [archive, group] : by_archive) {
        if (static_cast<int>(slots.size()) >= limit) break;
        if (archive == chosen_archive || group.empty()) continue;
        const size_t index = pick(group);
        slots.push_back(index);
        used[index] = 1;
    }

    std::vector<size_t> leftover;
    for (const auto& [archive, group] : by_archive) {
        for (size_t index : group) {
            if (!used[index]) leftover.push_back(index);
        }
    }
    std::sort(leftover.begin(), leftover.end(), [&](size_t left, size_t right) {
        const int model = CompareIgnoreCase(models[left].Model, models[right].Model);
        if (model != 0) return model < 0;
        return CompareIgnoreCase(ArchiveKey(models[left].Archive), ArchiveKey(models[right].Archive)) < 0;
    });
    for (size_t index : leftover) {
        if (static_cast<int>(slots.size()) >= limit) break;
        slots.push_back(index);
    }
    return slots;
}

}  // namespace skeleton_repair
