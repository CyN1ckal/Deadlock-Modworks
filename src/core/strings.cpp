#include "strings.hpp"

#include "error.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace skeleton_repair {

std::wstring Widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        throw InvalidData("A path or name is not valid UTF-8.");
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw InvalidData("A path or name is not valid Unicode.");
    }
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

namespace {

int CompareWide(std::wstring_view left, std::wstring_view right, bool ignore_case) {
    const int result = CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()),
        ignore_case ? TRUE : FALSE);
    if (result == 0) {
        throw IoError("String comparison failed.");
    }
    return result - 2;
}

}  // namespace

int CompareOrdinal(std::string_view left, std::string_view right) {
    return CompareWide(Widen(left), Widen(right), false);
}

int CompareIgnoreCase(std::string_view left, std::string_view right) {
    return CompareWide(Widen(left), Widen(right), true);
}

}  // namespace skeleton_repair
