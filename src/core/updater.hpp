#pragma once

#include "cancellation.hpp"
#include "report.hpp"

#include <functional>
#include <optional>
#include <string>

namespace skeleton_repair {

class RepairJournal;

struct UpdateProgress {
    int Completed = 0;
    int Total = 0;
    std::string Message;
};

// Analyzes hero rig resources and applies targeted repairs without replacing custom render rigs.
class HeroModelUpdater {
public:
    // `only_model` checks that resource and the hero model or animation skeleton with the same path.
    // An empty optional checks every hero resource in the archive.
    static UpdateReport Analyze(const std::string& game_directory, const std::string& mod_vpk,
        const std::function<void(const UpdateProgress&)>& progress = {}, CancellationToken cancellation = {},
        const std::optional<std::string>& only_model = {});

    static UpdateReport Update(const std::string& game_directory, const std::string& mod_vpk,
        const std::optional<std::string>& output_path, bool replace_source, bool make_backup,
        const std::function<void(const UpdateProgress&)>& progress = {}, CancellationToken cancellation = {},
        bool analyze_only = false, RepairJournal* journal = nullptr, const std::optional<std::string>& only_model = {});
};

}  // namespace skeleton_repair
