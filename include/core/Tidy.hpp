#pragma once
#include "core/Door.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Tidy -- the report (s015c, docs/TIDY.md). GTK-free, READ-ONLY: it reads the
// catalog (through catalog::query, read-only) and the top of home (opendir),
// and proposes. Nothing here moves anything -- s016 adds the acts.
//
// Every row says which NAMED QUERY proposed it. A query is SQL over the
// catalog; queries run in order and the first to claim a file wins (like
// rules). A file you decided "keep-here" is never proposed again. Nothing
// under the dev roots (~/Develop, ~/Projects, ~/src, ~/dev, ~/code) is
// proposed -- workspaces are Dev projects' (and Shelve's, s019), as Archive
// already treats them; they still count in folder health and the top of home.
//
//   short life  -- useful now, done soon: installers and disk images, old
//                  screenshots, reflex saves ("x (1).pdf", "Copy of x");
//   dormant     -- unused (neither opened nor changed) for 18 months: to the
//                  Archive, cataloged;
//   undecided   -- loose at the top of home or on the Desktop: no home yet;
//   active      -- everything else: counted, not listed.
//
// Plus: folder health (worst first), the top of home (each folder, what of
// yours is in it), and drift (XDG folders that point nowhere, no ~/Closet yet).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::tidy {

enum class Verdict { Short, Dormant, Undecided };
const char* verdict_name(Verdict v);   // "Short life", "Dormant", "Undecided"
const char* verdict_why(Verdict v);    // one line: what it means and where it would go (s016)

struct Query {
    std::string id;       // "installers" -- shown on each row it proposes
    std::string title;    // "Installers and disk images"
    Verdict     verdict;
    std::string sql;      // SELECT path, size, last_used ... (read-only; :now :short :dormant bound)
};
const std::vector<Query>& queries();   // in the order they claim

inline constexpr std::int64_t kShortAge   = 30LL * 86400;    // a short-life file, untouched this long
inline constexpr std::int64_t kDormantAge = 548LL * 86400;   // ~18 months (TIDY: tunable)

struct Row {
    std::string   rel;
    std::uint64_t bytes = 0;
    std::int64_t  last_used = 0;   // max(last read, last change)
    std::string   query;           // the query id that proposed it
    bool          package = false;
    std::uint64_t dev = 0, ino = 0;   // s016: identity as the Look saw it -- re-checked at plan time
    std::int64_t  changed = 0;        // ...and its last change (a file; a package's is its newest inside)
};
struct Group {
    Verdict             verdict;
    std::uint64_t       files = 0, bytes = 0;
    std::vector<Row>    rows;      // biggest first
};

struct Folder {                    // folder health
    std::string   rel;             // "" = the top of home
    std::uint64_t files = 0, bytes = 0;
    std::uint64_t arrived = 0;     // arrived (born, else first seen) in the last 30 days
    std::uint64_t kinds = 0;       // distinct kinds (content type's major part, else extension)
    std::uint64_t unread = 0;      // unused in a year
    double        score = 0;       // worse = higher; the line under it says why
    std::string   why;             // "12 arrived this month · 4 kinds mixed · 60% unused in a year"
};

struct Top {                       // one folder at the top of home
    std::string   name;
    std::uint64_t files = 0, bytes = 0;
    std::string   note;            // "code -- Dev projects'", "nothing of yours cataloged", ...
};

struct Report {
    bool                     ok = false;
    std::string              error;
    std::vector<Group>       groups;           // Short, Dormant, Undecided -- always all three
    std::uint64_t            files = 0, bytes = 0;      // your files in the catalog, there now
    std::uint64_t            active = 0, active_bytes = 0;
    std::uint64_t            kept = 0;         // decided keep-here, so not proposed
    std::vector<std::string> hands_off;        // s016a2: folders you said to leave alone
    std::uint64_t            left_alone = 0, left_alone_bytes = 0;   // your files under them (not active, never proposed)
    std::vector<Folder>      folders;          // worst first, the worst 12 that need it
    std::vector<Top>         top;              // the top of home, by size
    std::vector<std::string> loose;            // loose files at the top of home (names)
    std::vector<std::string> drift;            // one line each
    std::vector<std::string> notes;            // a query that failed, ... (said, never hidden)
};

// Build the report from the catalog in `data_dir` for `home` at `now`.
Report report(const std::string& data_dir, const std::string& home, std::int64_t now);

// "3 days", "5 months", "2 years" -- for "unused 2 years".
std::string age(std::int64_t seconds);

// ── s016: the acts ───────────────────────────────────────────────────────────
// Each card is ONE door step, previewed (the list on screen), undoable:
//   Short life -> ~/Closet/<name>, then remembered as "short" with an expiry
//                 (now + kShortAge) once it is there -- the aging review
//                 (s016b) offers it to the Trash after that;
//   Undecided  -> ~/Closet/<name> (gathered, to be sorted);
//   Dormant    -> ~/Archive/<year last changed>/<its path> (Archive's place).
// The Closet and its Junk Drawer are made in the same step if missing. A name
// already taken in the Closet gets " (from <folder>)"; if that is taken too
// the row is left out. Each row is looked at again first (same inode, a file's
// same mtime); anything changed or gone since the Look is left out and named.
inline constexpr const char* kCloset     = "Closet";
inline constexpr const char* kJunkDrawer = "Closet/Junk Drawer";

struct Act {
    door::Plan               plan;      // previewed stays false: only the UI sets it
    std::vector<std::string> dropped;   // "Documents/x.pdf: changed since the Look"
    std::vector<std::string> shorts;    // rels moved as short life -- remember() records them
    std::size_t              moves = 0;
    std::uint64_t            bytes = 0;
};
Act plan(const std::string& home, Verdict v, const std::vector<Row>& rows);

// After the door ran: each short-life row that is now in the Closet (gone from
// where it was, there at its `to`) gets the decision "short" with detail
// "expires <unix>". The catalog carries it along when the next Look sees the
// move. Returns how many were recorded. (Rouster's own file, not a user file.)
std::size_t remember(const std::string& data_dir, const std::string& home, const Act& act, std::int64_t now);
// "expires 1767225600" -> 1767225600; 0 if it isn't one.
std::int64_t expiry_of(const std::string& detail);

// The Closet itself, and where Downloads land (~/.config/user-dirs.dirs).
struct ClosetState {
    bool        closet = false, junk = false;   // ~/Closet, ~/Closet/Junk Drawer are folders
    bool        dirs_file = false;              // user-dirs.dirs exists
    std::string download;                       // XDG_DOWNLOAD_DIR relative to home ("Downloads"); "" = not set
    bool        redirected = false;             // download == "Closet"
};
ClosetState closet_state(const std::string& home);

// user-dirs.dirs text with XDG_DOWNLOAD_DIR="$HOME/Closet" (the line replaced,
// or added if missing). Pure -- the selftest pins it.
std::string redirect_downloads(const std::string& user_dirs_text);

// "Make the Closet": MakeDir ~/Closet and its Junk Drawer if missing; with
// `redirect`, Trash the old user-dirs.dirs and Write the new one in its place
// (Undo: the new one to the Trash, the old one back -- exactly as before).
// Screenshots can't be pointed: GNOME Shell saves into Pictures/Screenshots
// itself; the "screenshots" query gathers the old ones instead.
door::Plan closet_plan(const std::string& home, bool redirect);

}  // namespace rouster::core::tidy
