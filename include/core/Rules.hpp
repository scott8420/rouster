#pragma once
#include "core/Door.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Rules (s011, REQUIREMENTS §4.7) -- Hazel-style rules on watched folders.
// GTK-free, READ-ONLY: it matches and returns plans; the door acts.
//
// A rule watches ONE folder under home (Downloads is the inbox) and looks at
// the files at its top -- not into subfolders, not hidden files, not partial
// downloads (.part .crdownload ...), not anything changed in the last two
// minutes (still arriving). It matches when ALL of its tests hold:
//   name   a glob, case-insensitive ("*invoice*", "Screenshot*")
//   kinds  extensions ("pdf jpg")
//   age    untouched for at least N days
// (at least one test is required -- a rule never matches "everything").
// Then it does ONE thing: Move (to a folder, optionally renamed), Trash,
// Archive (~/Archive/<year>/<where it was>, like the Archive page), or File
// into a project (the project's pattern and routes, s010).
//
// Rules are ordered; a file goes to the FIRST rule that matches it.
// A rule is enabled only after its matches were shown (the edit page's
// preview); "Run rules now" shows every match again before anything moves.
// Q4 lean: rules run only while Rouster is open -- on "Run rules now", and,
// if "watch" is on, every 10 minutes (each run is one Activity step).
// Tags (GNOME metadata) are not in this milestone.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::rules {

enum class Do { Move, Trash, Archive, File };
const char* do_name(Do d);   // "move", "trash", "archive", "file"

struct Rule {
    std::string name;
    std::string folder = "Downloads";   // watched, under home
    std::string glob;                   // "" = any name
    std::vector<std::string> exts;      // lower case, no dot; empty = any
    int         older_days = 0;         // 0 = any age
    Do          act = Do::Move;
    std::string dest;                   // Move: a folder under home; File: a project folder under home
    std::string rename;                 // Move only: "" = keep; {stem} {ext} {date} {year} {yyyymmdd} {yyyymmdd-hhmm}
    bool        enabled = false;
};

struct Book {
    bool watch = false;                 // run enabled rules every 10 minutes while open
    std::vector<Rule> rules;
};

inline constexpr const char* kFile = "rules.json";   // in Rouster's data folder
inline constexpr std::int64_t kSettle = 120;         // seconds: younger files are still arriving

std::string to_json(const Book& b);
Book from_json(const std::string& text);
Book load(const std::string& file);                  // missing / bad -> empty
std::vector<Rule> starters();                        // suggestions, all off

bool glob_match(const std::string& pattern, const std::string& name);   // * ? ; case-insensitive
bool in_progress(const std::string& name);           // a partial download
std::string summary(const Rule& r);                  // "*.pdf older than 30 days -> move to ~/Documents/PDFs"
std::string problem(const Rule& r, const std::string& home);   // "" = it can run; else why not

struct Match {
    std::size_t   rule = 0;
    std::string   from, to;          // to: absolute; Trash: ""
    std::string   note;              // why it can't be done (then not checked)
    std::uint64_t bytes = 0, dev = 0, ino = 0;
    std::int64_t  mtime_ns = 0;
    bool          checked = true;
};
// READ-ONLY. Each file in each watched folder -> the first rule that matches.
// only: that rule alone, enabled or not (the edit page's preview); else the
// enabled rules that have no problem.
std::vector<Match> matches(const std::string& home, const std::vector<Rule>& rules, std::int64_t now,
                           std::optional<std::size_t> only = std::nullopt, std::size_t limit = 500);
// The checked matches: folders made as needed, then one Move / Trash each.
// Each is looked at again; changed, gone, or its place taken -> left out.
door::Plan plan(const std::string& home, const std::vector<Rule>& rules, const std::vector<Match>& ms,
                std::vector<std::string>& dropped);

}  // namespace rouster::core::rules
