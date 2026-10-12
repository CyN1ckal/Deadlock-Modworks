#pragma once

#include "cancellation.hpp"
#include "report.hpp"

#include <functional>
#include <optional>
#include <string>

namespace skeleton_repair {

class ContentStore;

struct UpdateProgress {
    int Completed = 0;
    int Total = 0;
    std::string Message;
};

// Analyzes hero rig resources and applies targeted repairs without replacing custom render rigs.
class HeroModelUpdater {
public:
    static UpdateReport Analyze(const std::string& game_directory, const std::string& mod_vpk,
        const std::function<void(const UpdateProgress&)>& progress = {}, CancellationToken cancellation = {});

    static UpdateReport Update(const std::string& game_directory, const std::string& mod_vpk,
        const std::optional<std::string>& output_path, bool replace_source, bool make_backup,
        const std::function<void(const UpdateProgress&)>& progress = {}, CancellationToken cancellation = {},
        bool analyze_only = false, ContentStore* store = nullptr);
};

}  // namespace skeleton_repair
