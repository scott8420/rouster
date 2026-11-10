#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "latch/Protect.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Scan -- the read-only walker (s002). GTK-free; the selftest drives it over a
// generated fixture, never the real home.
//
// What it promises (safety is the product):
//   * READ-ONLY. It opens directories and lstat()s entries. It never opens a
//     file's contents, never writes, never follows a symlink.
//   * ONE FILESYSTEM. A directory whose st_dev differs from the root's is not
//     entered (a mount, a FUSE portal, a nested btrfs subvolume or snapshot);
//     it is listed in Report::skipped so the user sees what was left out.
//   * DISK USAGE, not apparent size: st_blocks * 512, like du. A hard-linked
//     file is counted once.
//   * Paths are classified relative to HOME (REQUIREMENTS §2), so scanning a
//     subfolder classifies exactly as the same files would in a full scan.
//
// Threads: scan() runs N workers over a shared queue of directories and blocks
// until they finish. Call it from a worker thread; read Progress from any
// thread while it runs; set Progress::cancel to stop early.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::scan {

// REQUIREMENTS §2's four kinds, plus three the summary needs to be honest.
enum class Kind : int {
    Regenerable,   // ~/.cache, flatpak app caches, CMake/Ninja/Meson build dirs -- clean freely
    State,         // ~/.local/state, shell histories                           -- mostly safe, per item
    Settings,      // ~/.config, flatpak app config                             -- back up; orphans flagged only
    AppData,       // ~/.local/share, ~/.var/app                                -- back up; never auto-clean
    Trash,         // ~/.local/share/Trash                                      -- deleted already; Sweep empties it (s005)
    Protected,     // the never-touch list (§1.5)                               -- never acted on, ever
    Other,         // everything else: the user's own files
};
inline constexpr int kKindCount = 7;

const char* kind_name(Kind k);        // "Regenerable", ...
const char* kind_treatment(Kind k);   // the one-line "why" shown under the name

// The never-touch list, Samson's places, `*.samson` -- moved to latch
// (latch/Protect.hpp, 2026-11-10) so every app of ours refuses the same
// things. Rouster's names for them:
using ::latch::protect::never_touch;
using ::latch::protect::samson_places;
using ::latch::protect::set_owned;
using ::latch::protect::owned;
using ::latch::protect::protected_name;

// A directory's kind, given its path relative to home ("" = home itself), the
// kind of its parent, and whether it looks like a build tree (CMakeCache.txt,
// build.ninja or meson-info/ inside -- the caller checks; this stays pure).
// `rooted` is set when a rule fired here (the dir starts a new classified
// root); `expand` when its children should be listed as separate items
// (~/.cache -> ~/.cache/mozilla, ...).
struct DirClass { Kind kind; bool rooted; bool expand; };
DirClass classify_dir(const std::string& rel, Kind parent, bool looks_like_build);

// A plain file directly in a directory of kind `parent` (shell histories at the
// top of home are State).
Kind classify_file(const std::string& rel, Kind parent);

struct Bucket {
    std::uint64_t bytes = 0;   // disk usage
    std::uint64_t files = 0;
};

// One expandable row under a kind: a classified root (".cache/mozilla",
// "Develop/x/rouster/build").
struct Item {
    std::string   rel;         // relative to home
    Kind          kind;
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    // s015b, packages only: the newest change (mtime, anything inside) and the
    // newest READ (atime of the files inside -- never a folder's: our own
    // readdir would be the reader). 0 = none.
    std::int64_t  newest_change = 0;
    std::int64_t  newest_read   = 0;
};

// s015b: one of YOUR files, as the catalog sees it -- lstat / statx only,
// never opened. Collected only when asked (scan(..., &mine)). What counts:
// regular files of kind Other, not hidden (no `.` component), not inside a
// git repository (a folder holding .git -- Dev projects owns those) or
// node_modules; a Mac package is ONE entry (package = true), sized whole,
// its times the newest inside. Hard links: every name is listed.
struct Seen {
    std::string   rel;               // relative to home
    std::uint64_t bytes   = 0;       // disk usage
    std::int64_t  changed = 0;       // mtime (unix s)
    std::int64_t  read    = 0;       // atime (unix s)
    std::int64_t  born    = 0;       // statx birth time; 0 = the filesystem doesn't say
    std::uint64_t dev = 0, ino = 0;  // identity across a rename
    bool          package = false;
};

struct Report {
    std::string home, root;                      // what was scanned
    std::array<Bucket, kKindCount> by_kind{};
    std::vector<Item>        items;              // sorted by bytes, descending; Other not itemised
    std::vector<Item>        packages;           // s015a: Mac packages (.scriv ...), each ONE item, sized whole; biggest first
    std::vector<std::string> skipped;            // other-filesystem dirs, relative to home
    std::vector<std::string> unwalked;           // s015b: dirs that couldn't be opened, relative to home
    std::uint64_t errors = 0;                    // unreadable dirs / failed lstat
    std::uint64_t dirs   = 0;
    bool          cancelled = false;
    std::string   refused;                       // non-empty: the scan did not run, and why

    std::uint64_t total_bytes() const;
};

// Live counters, safe to read from another thread while scan() runs.
struct Progress {
    std::atomic<std::uint64_t> files{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> dirs{0};
    std::atomic<bool>          cancel{false};
    std::atomic<bool>          done{false};
};

// Walk `root` (which must be `home` or inside it) and classify everything.
// threads = 0 -> hardware_concurrency, capped at 8.
Report scan(const std::string& home, const std::string& root,
            Progress& progress, unsigned threads = 0);

// s015b: the same walk, also listing your files for the catalog into *mine
// (cleared first; unchanged when null). Directories are opened O_NOATIME
// where allowed, so the walk itself doesn't count as a read.
Report scan(const std::string& home, const std::string& root,
            Progress& progress, unsigned threads, std::vector<Seen>* mine);

// "1.2 GB", "340 MB", "12 KB", "0 B" -- decimal units, like GNOME Files.
std::string human_bytes(std::uint64_t b);

}  // namespace rouster::core::scan
