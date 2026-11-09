#pragma once
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

}  // namespace rouster::core::tidy
