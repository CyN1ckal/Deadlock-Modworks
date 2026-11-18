#pragma once

// Navigation icons from Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10), which share these code
// points. main merges whichever is installed into the body font; theme::IconsLoaded says whether it did. Without
// them the navigation shows its labels alone.
namespace skeleton_repair::icons {

inline constexpr unsigned short kFirst = 0xE700;
inline constexpr unsigned short kLast = 0xE9FF;

inline constexpr const char* kHome = "\xEE\xA0\x8F";
inline constexpr const char* kGlobe = "\xEE\x9D\xB4";
inline constexpr const char* kPerson = "\xEE\x9D\xBB";
inline constexpr const char* kPackage = "\xEE\x9E\xB8";
inline constexpr const char* kRepair = "\xEE\xA4\x8F";
inline constexpr const char* kZoom = "\xEE\x9C\x9E";
inline constexpr const char* kTwoPage = "\xEE\xA2\x9A";
inline constexpr const char* kHistory = "\xEE\xA0\x9C";
inline constexpr const char* kSettings = "\xEE\x9C\x93";
inline constexpr const char* kGame = "\xEE\x9F\xBC";

}  // namespace skeleton_repair::icons
