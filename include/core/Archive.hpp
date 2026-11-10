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
// s015a: a Mac package (.scriv ...) is ONE item wherever it sits, even at the
// top of home: old only when everything in it is old (hidden files too), and
// then moved whole. Never split -- no file inside one is offered on its own.
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
    bool          package = false;   // s015a: a Mac package (.scriv ...) -- one item, moved whole
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
    std::uint64_t packages = 0;      // s015a: packages met (kept whole: offered only when ALL of it is old)
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

// ── s018: search the archive (TIDY "The catalog": by name, kind, where it came
// from, when). READ-ONLY. The disk is the truth: ~/Archive is walked (lstat,
// no symlinks, one filesystem, hidden names skipped, a package one entry);
// the catalog adds what only it knows (the content type a Look gave it, the
// last read); Rouster's records (stored()) say where each came from and which
// Put back brings it home -- a file inside an archived folder comes back with
// its folder. Something in ~/Archive Rouster didn't put there is still found;
// its "from" is read off the Archive's own layout (Archive/<year>/<path>),
// and nothing offers to put it back.
const std::vector<std::string>& search_kinds();   // "Documents", "Pictures", "Music", "Video", "Text & code", "Archives", "Other"
// By content type first ("image/png" -> Pictures; "application/pdf" ->
// Documents), else by extension.
std::string search_kind(const std::string& ext, const std::string& content_type);

inline constexpr std::size_t kNoStored = static_cast<std::size_t>(-1);
inline constexpr std::size_t kIndexCap = 200000;   // entries walked at most

struct Entry {
    std::string   rel;               // "Archive/2019/Documents/Taxes/w2.pdf"
    std::string   name;              // "w2.pdf"
    std::string   from;              // "Documents/Taxes/w2.pdf" ("" = can't say)
    bool          recorded = false;  // `from` is from Rouster's records, not guessed from the layout
    std::size_t   stored = kNoStored;   // the Stored item (by index) whose Put back brings it home
    bool          inside = false;    // ...as part of an archived folder (the whole folder goes back)
    bool          package = false;
    std::string   ext, kind;         // kind: one of search_kinds()
    std::uint64_t bytes = 0;         // disk usage (a package: all of it)
    std::int64_t  changed = 0;       // mtime (a package: its newest inside)
    std::int64_t  read = 0;          // last read, from the catalog (0 = it doesn't know)
    std::int64_t  archived = 0;      // when it went in (the records; 0 = unknown)
    int           year = 0;          // the Archive/<year>/ it sits under (0 = none)
};
struct Index {
    bool               ok = false;
    std::string        error;
    std::vector<Entry> entries;      // by rel
    std::vector<int>   years;        // present, newest first
    std::uint64_t      bytes = 0;
    bool               truncated = false;   // past kIndexCap
    std::uint64_t      unread = 0;   // things that couldn't be read
};
Index index(const std::string& home, const std::string& data_dir, const std::vector<Stored>& stored);

struct Query {
    std::string text;                // words, any order, case-insensitive; each must be in the name or where it came from
    std::string kind;                // "" = any
    int         year = 0;            // 0 = any
    bool empty() const;
};
// Indices into ix.entries, best first: every word in the NAME before words
// only in the path, then the newest change. At most `limit`; `total` = all.
std::vector<std::size_t> match(const Index& ix, const Query& q, std::size_t limit, std::size_t* total = nullptr);

}  // namespace rouster::core::archive
