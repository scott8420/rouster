#pragma once
#include "core/Door.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Duplicates (s008, REQUIREMENTS §4.4). GTK-free, READ-ONLY: it walks, reads
// file contents to hash them, and asks the filesystem where a file's blocks
// live (FIEMAP). It never writes; the door acts.
//
// Detect: size -> head/tail hash -> full hash (xxHash XXH3-128, vendored).
//   1. walk home (one filesystem, no symlinks, no hidden entries, nothing
//      Protected, no build trees); hard links are one file, not duplicates;
//   2. files of the same size are candidates; 3. the first and last kEdge
//      bytes are hashed; 4. what still matches is hashed whole.
// Folders: a folder is a copy of another when every entry matches by name and
//   content, all the way down, and nothing in it was skipped (a hidden file,
//   a symlink, a protected or build folder: we can't vouch for what we didn't
//   read). Only the outermost copies are listed. A file inside a folder copy
//   is not listed again on its own.
// Keep: one member of each group is the keeper (§4.4 keep rules, in order):
//   not in Downloads, then not on the Desktop; a name that isn't "copy (2)";
//   older; shorter path. The UI can make another member the keeper.
// Act (the door): Share -- reflink dedupe, every path stays put (btrfs; the
//   default there); Trash -- a copy goes to the Trash, only when chosen.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::dups {

inline constexpr std::uint64_t kMinBytes = 4096;          // smaller file groups aren't listed (and can't share a block)
inline constexpr std::size_t   kEdge     = 16 * 1024;     // head / tail sample

enum class Mode { Share, Trash };

struct Member {
    std::string   rel;              // relative to home
    std::int64_t  mtime_ns = 0;
    std::uint64_t dev = 0, ino = 0, size = 0;   // as seen at Look (re-checked at plan time)
    std::uint64_t extents = 0;      // a fingerprint of where its blocks are (0 = unknown); equal = already shared
    bool          checked = false;  // act on this copy (ignored for the keeper)
};

struct Group {
    bool          folder = false;
    std::uint64_t bytes = 0;        // one copy's size (a folder: the sum of its files)
    std::uint64_t files = 1;        // files in one copy
    std::vector<Member> members;    // >= 2
    std::size_t   keep = 0;         // index of the keeper
    std::string   keep_why;         // the rule that chose it ("not in Downloads", ...)
    std::vector<std::string> inner; // folder: its files, relative to the folder (same in every copy)
    std::vector<std::uint64_t> inner_size;

    bool shared(std::size_t i) const;           // member i already shares the keeper's blocks
    std::uint64_t gain(Mode m) const;           // what the checked copies would free
};

struct Found {
    std::vector<Group> groups;      // folders first, then files; biggest gain first within each
    std::uint64_t files = 0;        // files looked at
    std::uint64_t hashed = 0;       // bytes read to hash
    std::uint64_t errors = 0;
    bool          reflink = false;  // home's filesystem can share blocks (btrfs) -> Share is the default
    bool          cancelled = false;
    std::string   refused;
};

struct Progress {
    std::atomic<std::uint64_t> files{0};    // walked
    std::atomic<std::uint64_t> hashed{0};   // bytes read
    std::atomic<int>           stage{0};    // 0 walk, 1 head/tail, 2 whole files, 3 folders
    std::atomic<bool>          cancel{false};
    std::atomic<bool>          done{false};
};
const char* stage_name(int stage);

// "report (2).pdf", "Copy of x", "x - Copy.txt", "x_copy" -> true.
bool copy_like(const std::string& name);

// Re-applies the keep rules to g (sets keep + keep_why). Public for the selftest.
void choose_keeper(Group& g);

// READ-ONLY. Walks `root` (home or inside it), hashes, groups. Checks are
// pre-set for `mode`: Share -> every copy not already shared; Trash -> none.
Found find(const std::string& home, const std::string& root, Progress& progress);

// Sets every copy's check for `mode` (the UI's mode switch).
void preset(Found& f, Mode mode);

// The checked copies as a door plan. Before anything is planned each keeper
// and copy is looked at again: anything changed since Look (size, mtime,
// inode) is left out and named in `dropped`. A group whose keeper would itself
// go to the Trash (inside a trashed folder) is left out whole. previewed stays
// false -- only the UI may set it.
door::Plan plan(const std::string& home, const Found& f, Mode mode, std::vector<std::string>& dropped);

// FIEMAP: both files' blocks are the same blocks (false when unknown).
bool same_blocks(const std::string& a, const std::string& b);

// statfs: btrfs (the one we trust to share blocks).
bool can_share(const std::string& path);

}  // namespace rouster::core::dups
