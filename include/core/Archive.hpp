#pragma once
#include "core/Door.hpp"
#include "core/Scan.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Archive (s009, REQUIREMENTS §4.6). GTK-free, READ-ONLY; the door acts.
//
// Files untouched for N months -> ~/Archive/<year>/<original relative path>,
// <year> = the year it was last changed. (Q6 lean: ~/Archive, inside home --
// the door's Move works, nothing is copied, Undo and Put back both work.)
//
// What is looked at: the folders at the top of home and their contents, and
// loose files at the top. NOT: ~/Archive itself; the dev roots (Develop,
// Projects, src, dev, code -- Dev projects owns them); hidden entries;
// anything Protected, Regenerable (caches, build trees) or in the Trash; any
// folder holding .git; node_modules; symlinks; other filesystems.
//
// A folder whose files are ALL old, with nothing in it skipped, is offered as
// one item (the outermost such folder) -- never a top-level folder itself
// (~/Documents is not archived whole). Other old files are offered one by one.
// Nothing is pre-checked (§1.6: "old" is a guess).
//
// Put back reads the door's manifest: every Move into ~/Archive that is still
// there (same inode) can go back to where it came from -- not only the last
// batch (§4.6: "the manifest makes every file restorable").
//
// Archiving MOVES; it frees no space (same filesystem). Compression would be
// a copy, and the door never copies -- later, if wanted.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::archive {

inline constexpr int kDefaultMonths = 12;
inline constexpr const char* kRoot = "Archive";   // relative to home

struct Item {
    std::string   rel;               // relative to home
    bool          folder = false;
    std::uint64_t bytes = 0, files = 1;   // disk usage
    std::int64_t  newest = 0;        // unix seconds: the newest file's mtime
    int           year = 0;          // -> ~/Archive/<year>/<rel>
    std::uint64_t dev = 0, ino = 0;  // re-checked at plan time
    std::int64_t  mtime_ns = 0;      // a file's mtime, re-checked
    bool          checked = false;
};

struct Found {
    std::vector<Item> items;         // by top folder, then biggest first
    std::uint64_t files = 0;         // files looked at
    std::uint64_t old_bytes = 0;     // the total of items
    std::uint64_t errors = 0;
    bool          cancelled = false;
    std::string   refused;
};

// Dev roots Archive leaves to Dev projects.
const std::vector<std::string>& dev_roots();

// "Documents/x.pdf", 2019 -> "Archive/2019/Documents/x.pdf"
std::string destination(const std::string& rel, int year);

// READ-ONLY. `now` unix seconds (a parameter so the selftest pins it).
Found find(const std::string& home, std::int64_t now, int months, scan::Progress& progress);

// The checked items: the folders they need (MakeDir, parents first), then one
// Move each. Each item is looked at again first; anything changed or gone, or
// whose place in the archive is taken, is left out and named in `dropped`.
// previewed stays false.
door::Plan plan(const std::string& home, const std::vector<Item>& items, std::vector<std::string>& dropped);

// What is in the archive, from the manifest.
struct Stored {
    std::string   rel;               // "Archive/2019/Documents/x.pdf"
    std::string   back;              // "Documents/x.pdf"
    bool          folder = false;
    std::uint64_t bytes = 0;
    std::int64_t  when = 0;          // archived at
    bool          blocked = false;   // something is at `back` now
    bool          checked = false;
};
std::vector<Stored> stored(const door::Door& door, const std::string& home);

// The checked stored items back where they came from (MakeDir for missing
// folders, then Move). A blocked one is left out and named.
door::Plan put_back(const std::string& home, const std::vector<Stored>& items, std::vector<std::string>& dropped);

}  // namespace rouster::core::archive
