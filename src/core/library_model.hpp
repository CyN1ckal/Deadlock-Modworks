#pragma once

#include "content_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

std::string FormatModelRow(const ModelRow& row);
std::vector<std::string> FormatHistory(const History& history);
std::string FormatUnproven(const std::vector<SlotObservation>& slots);
std::vector<ModelRow> FilterModels(const std::vector<ModelRow>& rows, std::string_view query);

}  // namespace skeleton_repair
