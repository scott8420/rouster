#pragma once
#include "latch/Door.hpp"
#include "core/Git.hpp"
#include "core/Helper.hpp"

#include <string>
#include <vector>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────────────────
// Door -- THE ONE DOOR now lives in latch (latch/Door.hpp, 2026-11-10), shared
// with Tidy. Everything said there holds; this shim keeps Rouster's name for it
// and adds what is Rouster's own: the root helper (its path, its requests, what
// it says) and the app's name in the door's refusals and staging names.
//
// install() runs once at startup -- in the app before the first Door, in the
// selftest before the first test. Without it the door refuses every Root step
// and says "the app" instead of "Rouster".
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::door {
using namespace ::latch::door;

inline void install() {
    ::latch::door::set_app_name("Rouster");
    ::latch::door::set_root_rules({
        helper::kHelperPath,
        [](const std::string& request) { return helper::parse(request).has_value(); },
        [](const std::string& out) {
            std::vector<std::string> notes;
            for (const auto& l : helper::parse_output(out)) notes.push_back(l.detail);
            return notes;
        },
    });
}

// {"/usr/bin/pkexec", kHelperPath, "run", ["--dry-run",] request}
inline std::vector<std::string> root_argv(const std::string& request, bool dry) {
    return ::latch::door::root_argv_for(helper::kHelperPath, request, dry);
}
// Is the helper installed where the polkit action says it is?
inline bool helper_installed() { return ::access(helper::kHelperPath, X_OK) == 0; }

}  // namespace rouster::core::door
