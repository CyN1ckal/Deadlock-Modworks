#pragma once

#include <string>
#include <string_view>

namespace skeleton_repair {

std::wstring Widen(std::string_view text);
std::string Narrow(std::wstring_view text);

// Compares strings in UTF-16 code unit order.
int CompareOrdinal(std::string_view left, std::string_view right);

// Compares strings using Windows ordinal case-insensitive ordering.
int CompareIgnoreCase(std::string_view left, std::string_view right);

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right) {
    return CompareIgnoreCase(left, right) == 0;
}

}  // namespace skeleton_repair
