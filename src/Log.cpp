#include "Log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <string>

namespace rouster::log {

namespace {
constexpr int kAreaCount = 17;

const char* area_name(Area a) {
    switch (a) {
        case Area::App:      return "app";
        case Area::Shell:    return "shell";
        case Area::Registry: return "registry";
        case Area::Recents:  return "recents";
        case Area::Io:       return "io";
        case Area::Scan:     return "scan";
        case Area::Preflight: return "preflight";
        case Area::Door:     return "door";
        case Area::Sweep:    return "sweep";
        case Area::Dev:      return "dev";
        case Area::Dups:     return "dups";
        case Area::Archive:  return "archive";
        case Area::Organize: return "organize";
        case Area::Rules:    return "rules";
        case Area::Profile:  return "profile";
        case Area::System:   return "system";
        case Area::Catalog:  return "catalog";
    }
    return "?";
}

std::array<std::shared_ptr<spdlog::logger>, kAreaCount>& loggers() {
    static std::array<std::shared_ptr<spdlog::logger>, kAreaCount> a{};
    return a;
}
}  // namespace

void init() {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto& a = loggers();
    for (int i = 0; i < kAreaCount; ++i) {
        const auto area = static_cast<Area>(i);
        auto lg = std::make_shared<spdlog::logger>(area_name(area), sink);
        lg->set_level(spdlog::level::info);   // default INFO; raise one area to TRACE to debug
        a[static_cast<std::size_t>(i)] = lg;
    }
}

std::shared_ptr<spdlog::logger> get(Area area) {
    return loggers()[static_cast<std::size_t>(area)];
}

void set_level(Area area, spdlog::level::level_enum level) {
    if (auto lg = get(area)) lg->set_level(level);
}

}  // namespace rouster::log
