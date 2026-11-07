#pragma once
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// About (s014a2) -- what the About window says, as data: the app's facts and
// the credits. GTK-free so the selftest can hold the credits to the tree:
// every file in third_party/ is named under "Built with".
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::about {

struct Credit {
    std::string name;      // "nlohmann/json"
    std::string what;      // "JSON for the records and settings"
    std::string license;   // "MIT"
    std::string url;       // "" = none
    std::string vendored;  // the third_party/ file it is, "" = linked / used, not vendored
};

struct Group {
    std::string heading;
    std::vector<Credit> items;
};

inline constexpr const char* kName = "Rouster";
inline constexpr const char* kAppId = "io.github.scott8420.Rouster";
inline constexpr const char* kWebsite = "https://github.com/scott8420/rouster";
inline constexpr const char* kLicense = "MIT License";
inline constexpr const char* kCopyright = "© 2026 Scott (scott8420)";
const char* tagline();
const char* description();
const std::vector<Group>& credits();   // Made by, Built with, Works with, Inspired by

}  // namespace rouster::core::about
