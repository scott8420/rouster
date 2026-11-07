#pragma once
#include "core/Door.hpp"
#include "core/Helper.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// System (s013b, REQUIREMENTS §4.2 "Root (via helper)") -- the three things
// outside home Rouster may clean, each one request to the root helper:
// the package cache, the journal, an SSD trim. find() measures WITHOUT root
// (sizes it can't read are said to be "at least"); plan() turns the checked
// rows into Root actions, unpreviewed -- only the UI sets previewed.
// Two of them can't be undone (the cache refills, old logs are gone); the
// page says so on the row and on the button.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::sysclean {

struct Item {
    helper::Request req;
    std::string   title;        // "Package cache"
    std::string   why;          // the one line under it
    std::string   note;         // e.g. "Fedora already trims weekly"
    std::uint64_t bytes = 0;    // what it holds now (trim: 0)
    bool          sized = false;      // bytes means something
    bool          complete = true;    // every part could be read
    bool          available = true;   // the program is here
    bool          checked = false;    // the preset (safe -> checked)
};

// Where to look (the selftest points these at a fixture; the app uses the real ones).
struct Where {
    std::vector<std::string> pkg_dirs = helper::pkg_cache_dirs();
    std::string journal = helper::kJournalDir;
    std::vector<std::string> pkg_programs = {"/usr/bin/dnf", "/usr/bin/apt-get"};
    std::vector<std::string> journal_programs = {"/usr/bin/journalctl", "/bin/journalctl"};
    std::vector<std::string> trim_programs = {"/usr/sbin/fstrim", "/sbin/fstrim", "/usr/bin/fstrim"};
    std::string trim_timer = "/etc/systemd/system/timers.target.wants/fstrim.timer";
};

std::vector<Item> find(const Where& w = {}, int journal_days = helper::kJournalDefault);
door::Plan plan(const std::vector<Item>& items);   // checked + available, in order; previewed = false

}  // namespace rouster::core::sysclean
