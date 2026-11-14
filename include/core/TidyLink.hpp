#pragma once
#include "core/Door.hpp"
#include <string>
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// TidyLink (s019d) -- what Rouster reads from Tidy, and what of Tidy's it
// never touches (docs/TIDY_FOR_ROUSTER.md §2, §4). READ-ONLY: Tidy's files are
// Tidy's; nothing here writes (the source gate holds it).
//
//   hands_off()  -- Tidy's Hands off list: the `hands_off=<rel>` lines of
//                   ~/.local/share/tidy/settings.ini, relative to home. ONE
//                   list for both apps: Rouster has none of its own. At startup
//                   they join Samson's places as latch's owned places, so they
//                   are Protected all the way down -- no Look lists them, no
//                   module proposes them, and the door refuses them.
//   is_mark()    -- `.tidy-files` (a folder's id list) and `.tidy-project`
//                   (a project's id): never moved, trashed or paired on their
//                   own. A folder holding one may still move whole.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::tidylink {

inline constexpr const char* kSettings = ".local/share/tidy/settings.ini";   // relative to home

// The hands_off entries of <home>/.local/share/tidy/settings.ini, relative to
// home: "~/" or "<home>/" taken off, trailing "/" trimmed, duplicates once.
// Left out: empty, ".." anywhere, absolute paths outside home, home itself.
// No file (Tidy not installed) -> empty.
std::vector<std::string> hands_off(const std::string& home);

// The same, from the text of a settings.ini (the parser, for the selftest).
std::vector<std::string> parse_hands_off(const std::string& home, const std::string& text);

// s019d2: Rouster's own folder (~/Rouster, where its profile backups go). Tidy's
// Hands off says "Tidy, leave it alone" -- Scott marks ~/Rouster that way -- but
// read by Rouster it would make Rouster's own folder Protected and the door would
// refuse its backups. So for_rouster() leaves out ~/Rouster and anything under it.
inline constexpr const char* kOwnRoot = "Rouster";
std::vector<std::string> for_rouster(std::vector<std::string> hands_off);

// A name Tidy keeps beside your files: ".tidy-files", ".tidy-project".
bool is_mark(std::string_view name);

// Rouster's last look at a plan before the door (Shell::door_run, the only
// caller of Door::run in the app): "" = it may run; else why not. Refused:
// any step whose source or target IS one of Tidy's marks, and any Tag step
// (user.tidy.* is Tidy's -- Rouster never tags). A folder holding a mark
// moving whole is fine.
std::string guard(const door::Plan& plan);

}  // namespace rouster::core::tidylink
