#pragma once

#include "library_types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

// The hero folder in models/heroes/<character>/... or models/heroes*/<character>/... (for example heroes_staging).
// Empty when the model is not stored under a hero folder.
std::optional<std::string> CharacterOfModel(std::string_view model_path);

// File name without .vmdl_c, with underscores shown as spaces.
std::string ModelFileTitle(std::string_view model_path);

// "archive.vpk - mina ult", so two mods of one model can be told apart.
std::string SlotCaption(const LibraryModel& model);

struct ViewportGrid {
    int Columns = 1;
    int Rows = 1;
};

// 1 is one pane, 2 and 3 are a row, 4 is a 2x2 grid. Counts outside 1..4 clamp into that range.
ViewportGrid GridForViewportCount(int count);

struct CharacterOption {
    std::string Name;
    size_t Count = 0;
};

// One entry per character, sorted A to Z, using the first spelling found in the library.
std::vector<CharacterOption> CharactersIn(const std::vector<LibraryModel>& models);

// Fills up to `limit` slots with models of the chosen model's character.
// The chosen model is first. Other mods of that character come next, preferring each mod's copy of the
// same model file, and further files from a mod already represented fill any slots that remain.
std::vector<size_t> CharacterSlots(const std::vector<LibraryModel>& models, size_t chosen, int limit);

}  // namespace skeleton_repair
