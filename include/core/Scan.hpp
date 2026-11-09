#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

// The never-touch list, relative to home. Code, not configuration (§1.5).
// Any directory named `fixes` is protected as well, wherever it is.
const std::vector<std::string>& never_touch();

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
};

struct Report {
    std::string home, root;                      // what was scanned
    std::array<Bucket, kKindCount> by_kind{};
    std::vector<Item>        items;              // sorted by bytes, descending; Other not itemised
    std::vector<Item>        packages;           // s015a: Mac packages (.scriv ...), each ONE item, sized whole; biggest first
    std::vector<std::string> skipped;            // other-filesystem dirs, relative to home
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

// "1.2 GB", "340 MB", "12 KB", "0 B" -- decimal units, like GNOME Files.
std::string human_bytes(std::uint64_t b);

}  // namespace rouster::core::scan
