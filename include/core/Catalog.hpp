#pragma once
#include "core/Scan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// The catalog (s015b, docs/TIDY.md) -- Rouster's MEMORY of your files.
// GTK-free. `<data_dir>/catalog.db`, SQLite.
//
//   * The disk is the truth, the catalog is memory. Nothing here is ever a
//     reason to act without the door's own re-check of the file.
//   * Paths (relative to home) and times, never contents. Rouster's own file,
//     like rules.json -- the source gate allows only Catalog.cpp to open a
//     database.
//   * Rebuildable: delete it and the next Look builds it again -- all but the
//     history and the decisions, which only it remembers. A database that
//     won't open, or isn't ours, is set aside (catalog.db.broken-<stamp>),
//     never deleted, and a fresh one begins.
//
// What it remembers that the filesystem can't say:
//   * first seen -- when a Look first met the file (arrival; Closet aging
//     counts from it). `born` is statx's birth time where the filesystem
//     keeps one (btrfs, ext4) -- the better guess on a fresh catalog;
//   * history -- last read / last change / size, a row each time a Look saw
//     one of them move;
//   * moves -- a file gone from one path and found at another with the same
//     inode, size and change time is the SAME file: its first seen, history
//     and decisions follow it;
//   * gone -- a file no longer there keeps its row (and its history), marked
//     with when it was missed; back again -> "returned";
//   * decisions -- verdicts you gave (s015c on), so nothing is asked twice.
//
// Every call opens the database, does its work in one transaction, and closes
// it. Safe from a worker thread; two at once wait (busy timeout).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::catalog {

inline constexpr const char* kFile = "catalog.db";
inline constexpr int kSchema = 1;

// What one Look changed in the catalog.
struct Delta {
    bool          ok = false;
    std::string   error;          // !ok: why nothing was recorded
    std::string   set_aside;      // a broken / foreign catalog moved aside to this path (a fresh one began)
    bool          began = false;  // this Look started the catalog
    std::int64_t  look = 0;       // its id
    std::uint64_t files = 0;      // your files there now (in this Look's scope), packages included
    std::uint64_t packages = 0;
    std::uint64_t fresh = 0;      // first seen this Look
    std::uint64_t changed = 0;    // changed since the last Look (mtime or size)
    std::uint64_t read = 0;       // opened, not changed (atime only)
    std::uint64_t moved = 0;      // same file, new path
    std::uint64_t gone = 0;       // missed this Look
    std::uint64_t returned = 0;   // gone before, here again
    std::uint64_t rows = 0;       // everything the catalog holds, gone included
    std::string   atime_mode;     // home's mount: "relatime", "noatime", "strictatime", "" unknown
};

// Record one Look. `root_rel` is what was scanned, relative to home ("" =
// all of it): only rows under it can be missed. `not_walked` (unreadable or
// other-filesystem folders, relative to home) can't be missed either --
// nobody looked. `now` = unix seconds.
Delta record(const std::string& data_dir, const std::string& home, const std::string& root_rel,
             const std::vector<scan::Seen>& seen, const std::vector<std::string>& not_walked,
             std::int64_t now);

struct Summary {
    bool          ok = false;          // false: no catalog yet (or it won't open)
    std::uint64_t files = 0, packages = 0, gone = 0, looks = 0, decisions = 0;
    std::int64_t  began = 0, last_look = 0;
};
Summary summary(const std::string& data_dir);

struct File {
    std::int64_t  id = 0;
    std::string   rel;
    bool          package = false;
    std::string   ext;                 // lowercased, no dot ("" = none)
    std::uint64_t bytes = 0;
    std::int64_t  first_seen = 0, born = 0, read = 0, changed = 0;
    std::int64_t  gone = 0;            // 0 = there at the last Look
};
std::optional<File> find(const std::string& data_dir, const std::string& rel);

struct Moment { std::int64_t look_at = 0, read = 0, changed = 0; std::uint64_t bytes = 0; };
std::vector<Moment> history(const std::string& data_dir, const std::string& rel);   // oldest first

struct Move { std::int64_t at = 0; std::string from, to; };
std::vector<Move> moves(const std::string& data_dir, const std::string& rel);       // oldest first

// Decisions. A verdict is a short lowercase word (a-z and '-', <= 32):
// "keep-here", "short", "active", "dormant", "undecided", "home" ... -- the
// Tidy report (s015c) defines the set. The newest decision for a path wins;
// they follow the file when it moves.
struct Decision { std::int64_t id = 0, at = 0; std::string rel, verdict, detail; };
bool valid_verdict(const std::string& v);
bool decide(const std::string& data_dir, const std::string& rel, const std::string& verdict,
            const std::string& detail, std::int64_t now, std::string* why = nullptr);
std::vector<Decision> decisions(const std::string& data_dir);                      // newest first

// How the filesystem holding `path` keeps atime, from /proc/self/mountinfo:
// "noatime", "relatime", "strictatime" (neither flag), or "" when unknown.
std::string atime_mode(const std::string& path);

}  // namespace rouster::core::catalog
