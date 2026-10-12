#pragma once

#include <string>
#include <vector>

namespace skeleton_repair {

// Runs the analyze-then-repair window until it closes. Paths in initial_mods are preselected.
int RunMainWindow(const std::vector<std::string>& initial_mods);

}  // namespace skeleton_repair
