#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Packages are one thing (s015a, docs/TIDY.md). GTK-free, READ-ONLY.
//
// A Mac package is a folder the Mac shows as one file: a Scrivener project
// (`.scriv` -- Scott's novels, the one that matters), Pages / Numbers /
// Keynote documents, `.rtfd`, `.app`, a Photos library. On Linux it is just a
// folder, so every walker in Rouster would happily look inside, split it, or
// pair files across two copies. The rule, everywhere (Smart Scan, Sweep,
// Duplicates, Archive, Rules -- and the door, as the backstop):
//
//   never looked inside, never split, never compared or trashed piecemeal;
//   the only act on one is MOVING IT WHOLE, with Undo.
//
// A package is a DIRECTORY whose name ends in one of kinds() (any case) and
// has something before the dot. A file named `server.key` is not a package.
// Only the OUTERMOST package counts (a .scriv inside a .scriv is part of it).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::pkg {

// ".scriv", ".scrivtemplate", ".pages", ".numbers", ".key", ".rtfd", ".app",
// ".photoslibrary" -- the list is code, like the never-touch list.
const std::vector<std::string>& kinds();

// The name alone (no lstat): "Tapper.scriv", "TAPPER.SCRIV" -> true;
// ".scriv", "notes.txt", "scriv" -> false. Callers that have only a name of
// a thing that may be a file must also check it is a directory.
bool is_package_name(std::string_view name);

// The outermost package a path is strictly INSIDE (an ancestor component
// whose name is a package's), as the prefix up to and including it;
// "" when none. Works on relative or absolute paths. Ancestors are folders
// by definition, so this needs no lstat.
//   "Novels/Tapper.scriv/Files/Data/x.rtf" -> "Novels/Tapper.scriv"
//   "Novels/Tapper.scriv"                   -> ""  (the package itself)
std::string enclosing(std::string_view path);

// The outermost package a path IS or is inside: like enclosing(), but the
// last component counts too (the caller vouches it is a directory).
std::string package_of(std::string_view path, bool last_is_dir);

// A whole package, measured without looking at contents: lstat only, one
// filesystem, symlinks not followed, hidden entries included (they are part
// of it). `newest` = the newest mtime of anything inside, the package's own
// included.
struct Measure {
    std::uint64_t bytes = 0, files = 0;
    std::int64_t  newest = 0;   // unix seconds
    std::uint64_t errors = 0;
};
Measure measure(const std::string& abs);

}  // namespace rouster::core::pkg
