#pragma once
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helper (s013, REQUIREMENTS §1.4) -- what Rouster and its small ROOT helper
// agree on. The helper (src/helper/main.cpp, installed by `install.sh
// --helper` at kHelperPath) is run through pkexec with a FIXED VERB LIST: no
// path, no command, no shell ever crosses from the app. Each request is one
// word and at most one number, read here by both sides; the helper checks
// everything again for itself -- it is the security boundary, the app is not.
//
// This file is pure (reads only: stat / statfs / readdir), GTK-free, and
// links into BOTH rouster_core and the helper, so the two can't drift.
//
//   snapshot:N   a read-only btrfs snapshot of the subvolume holding the
//                caller's home, in <subvolume>/.rouster-snapshots/home-<stamp>;
//                then Rouster's own snapshots beyond the newest N are deleted.
//                Nothing else in that folder is ever touched.
//   pkgclean     (s013b) the package cache: `dnf clean all` (Fedora), else
//                `apt-get clean` (Ubuntu). Can't be undone; it refills.
//   journal:N    (s013b) `journalctl --vacuum-time=Nd` -- archived journal
//                files older than N days go (7..365). Can't be undone.
//   trim         (s013b) `fstrim --all --verbose`. Changes no file.
// The helper runs those three programs by absolute path, with a clean
// environment and no shell; nothing from the app reaches their argv but N.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::helper {

inline constexpr const char* kHelperPath = "/usr/local/libexec/rouster/rouster-helper";
inline constexpr const char* kPolicyId   = "io.github.scott8420.Rouster.helper";
inline constexpr const char* kSnapDir    = ".rouster-snapshots";
inline constexpr int kKeepDefault = 5, kKeepMin = 1, kKeepMax = 20;
inline constexpr int kJournalDefault = 28, kJournalMin = 7, kJournalMax = 365;   // days kept

enum class Op { Snapshot, PkgClean, Journal, Trim };
const char* op_name(Op o);   // "snapshot", "pkgclean", "journal", "trim"
const char* op_title(Op o);  // "Snapshot", "Package cache", "System journal", "SSD trim"
bool undoable(Op o);         // false for all: Undo closes a root step (a snapshot just isn't taken back)
bool irreversible(Op o);     // PkgClean, Journal: what they remove is gone

struct Request {
    Op  op  = Op::Snapshot;
    int arg = kKeepDefault;  // snapshot: how many of Rouster's to keep; journal: days kept; else 0
    bool operator==(const Request&) const = default;
};
// "snapshot:5", "journal:28", "pkgclean", "trim" -> Request; anything else (unknown word, out of range, junk,
// a second colon, a sign, spaces) -> nullopt. Strict on purpose.
std::optional<Request> parse(const std::string& s);
std::string text(const Request& r);          // the inverse: "snapshot:5"
std::string describe(const Request& r);      // "Snapshot of /home (keeps the newest 5)"

// Snapshot names: "home-20261106-233400" (UTC -- pkexec clears TZ, so "local"
// could differ between the helper and the app). Sort = age.
std::string snapshot_name(std::time_t t);
bool is_snapshot_name(const std::string& s);
std::optional<std::time_t> snapshot_time(const std::string& name);
// Of `names` (any order, any entries), Rouster's snapshots to delete so the
// newest `keep` stay. Names that aren't Rouster's are never in the answer.
std::vector<std::string> to_expire(std::vector<std::string> names, int keep);

// Disk use of everything under `dir` (st_blocks, one filesystem, no links
// followed, unreadable parts skipped -- `complete` says whether any were).
struct Usage { std::uint64_t bytes = 0; bool complete = true; bool exists = false; };
Usage du(const std::string& dir);
// The package caches (whichever exist): /var/cache/libdnf5, /var/cache/dnf, /var/cache/apt/archives.
const std::vector<std::string>& pkg_cache_dirs();
// The journal: /var/log/journal (persistent).
inline constexpr const char* kJournalDir = "/var/log/journal";
// The first of `paths` that is an executable file, or "".
std::string first_program(const std::vector<std::string>& paths);
// "1.2 GB" (powers of 1000, like the rest of Rouster).
std::string human(std::uint64_t bytes);

// Where a snapshot of `home` goes. ok=false + why when it can't be taken:
// not btrfs, or home shares the subvolume mounted at / (a snapshot would take
// the whole system), or home can't be read.
struct Place {
    bool        ok = false;
    std::string subvol;   // the subvolume's top folder holding home ("/home")
    std::string dir;      // subvol + "/.rouster-snapshots"
    std::string why;
};
Place place_for(const std::string& home);

// Rouster's snapshots already in `dir`, newest first (names only; reads the folder).
std::vector<std::string> list_snapshots(const std::string& dir);

// One result line on the helper's stdout:   rouster-helper <TAB> op <TAB> ok|failed|dry <TAB> detail
struct Line {
    std::string op, status, detail;
    bool operator==(const Line&) const = default;
};
std::string format_line(const Line& l);      // tabs / newlines in detail become spaces
std::optional<Line> parse_line(const std::string& s);
std::vector<Line> parse_output(const std::string& out);

}  // namespace rouster::core::helper
