#pragma once
#include "core/Door.hpp"
#include "core/Scan.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Sweep (s006, REQUIREMENTS §4.2) -- the first module that acts, and it acts
// only the way every module will: it FINDS (read-only), it builds a PLAN, the
// UI shows the plan, and the door runs it. Nothing in this file moves a file.
//
// What it offers (user-level only; root items wait for the helper, s013):
//   Caches      -- each child of ~/.cache.                       pre-checked
//                  except the slow-to-rebuild ones (model and
//                  browser-engine downloads, IDE indexes)        unchecked
//   Thumbnails  -- ~/.cache/thumbnails, named apart.             pre-checked
//   App caches  -- ~/.var/app/<id>/cache (Flatpak).              pre-checked
//   Old logs    -- *.log, *.log.N, *.log.gz ... under
//                  ~/.local/state and ~/.local/share, untouched
//                  for kOldLogDays.                              unchecked (§1.2: doubtful)
//   The Trash   -- measured and shown; NOT offered. Emptying it is the one
//                  irreversible act and the door has no verb for it (§1.1).
//
// Every candidate came out of the scan's classifier, so Protected (never-touch,
// any fixes/) can't appear: a protected dir is never Regenerable. The log walk
// applies the same list itself. The door re-checks all of it anyway.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::sweep {

enum class Group { Thumbnails, Caches, AppCaches, Logs };
inline constexpr int kGroupCount = 4;
const char* group_name(Group g);    // "Thumbnails", ...
const char* group_why(Group g);     // the one line under the group

inline constexpr int kOldLogDays = 30;

struct Candidate {
    std::string   rel;              // relative to home
    Group         group = Group::Caches;
    std::uint64_t bytes = 0, files = 0;
    bool          checked = true;   // the pre-check (§1.2)
    std::string   note;             // why it is unchecked, if it is
};

struct Found {
    std::vector<Candidate> items;   // groups in enum order, biggest first within a group
    std::uint64_t trash_bytes = 0, trash_files = 0;
    std::uint64_t errors = 0;
    bool          cancelled = false;
    std::string   refused;          // the scan refused home (see scan::scan)
};

// Caches whose loss costs a long download or a long reindex. Unchecked, with
// that reason. Names are ~/.cache children.
const std::vector<std::string>& slow_to_rebuild();

// "app.log", "app.log.1", "app.log.gz", "Xorg.0.log.old" -> true.
bool is_log_name(const std::string& name);

// READ-ONLY. Walks ~/.cache and ~/.var/app with the scanner, then the log
// folders and the Trash with lstat only. `now` is unix seconds (the log age
// cut-off; a parameter so the selftest can pin it). Progress is the scanner's.
Found find(const std::string& home, scan::Progress& progress, std::int64_t now);

// The checked candidates as a door plan: Trash each one. previewed stays
// false -- only the UI, having shown the list, may set it (RULES: preview first).
door::Plan plan(const std::string& home, const std::vector<Candidate>& items);

}  // namespace rouster::core::sweep
