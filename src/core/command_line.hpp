#pragma once

#include "cancellation.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace skeleton_repair {

class CommandLine {
public:
    // One line per call, without a trailing newline. Returns 0, 1, or 2.
    static int Execute(const std::vector<std::string>& args, const std::function<void(std::string_view)>& write,
        CancellationToken cancellation = {});
};

}  // namespace skeleton_repair
