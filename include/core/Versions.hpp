#pragma once
#include "core/Door.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Version families (s017, docs/TIDY.md "Version families"). GTK-free,
// READ-ONLY: it reads the catalog (read-only), lstat()s, and reads bytes to
// compare two saves; the door acts.
//
// "Save, save, save -- which one is the good one?" A FAMILY is two or more of
// your files in ONE folder with the same extension whose names are the same
// once the version marks are taken off the end:
//
//   ch3.odt  ch3 final.odt  ch3 final2.odt  ch3 (1).odt  Copy of ch3.odt
//   ch3_v2.odt  ch3 draft 3.odt  ch3 - Copy.odt                  -> "ch3"
//
// What is NOT a mark, on purpose: a bare number ("Chapter 2" and "Chapter 3"
// are two chapters) and a date ("Invoice 2026-01" and "Invoice 2026-02" are
// two invoices). Names only -- similar content under unrelated names is
// Duplicates' question, not this one.
//
// THE NEWEST IS THE KEEPER and stays put; the rest go to "<base> versions/"
// right beside it (`Tapper.odt` + `Tapper versions/`). A save already in that
// folder (or in a plain `versions/` -- s010's New version) is a member, sorted.
// One member outside the versions folder = a tidy family: it is the keeper
// whatever its age. Several outside: the one you chose (decision "keeper"),
// else the newest.
//
// REFLEX SAVES among the older ones are marked (suggested short life):
// a copy's name ("x (1)", "Copy of x"), the same bytes as another member, or
// saved minutes before the next one at nearly the same size.
//
// Never proposed: dev roots, the Closet, the Archive, hands-off folders,
// inside a Projects folder (s010 -- it keeps its own versions), and
// APP-MANAGED HISTORY, which is recognised and left alone:
//   Folio  -- <data>/folio/backups/<stem>/<stem>_YYYY-MM-DDTHH-MM.folio.gz
//             (or a .folio bundle copy); `backup-dir` in
//             <config>/folio/preferences.ini [AutoSave] moves it;
//   jot    -- <data>/jot/backups/<slot>/YYYY-MM-DD/ (rsync hard-link days),
//             `.source` names the jots folder; `backup_dir` in
//             <data>/jot/prefs.json moves it;
//   Trails -- <data>/trails/recordings/*.trail (recordings, not versions).
//   Samson -- its whole folder (~/Samson or samson_dir), PROTECTED by the
//             scan (s017a2) -- listed so you see why nothing there is proposed.
// Curvz, Optik, Quire and Mortimer keep no history of their own (read from
// their code at s017); Folio's snapshots live INSIDE its bundle (a package).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::versions {

// "ch3 final2" -> "ch3"; "Copy of ch3" -> "ch3". The stem only (no extension).
// `marked` (optional) says whether anything came off; `copyish` whether a
// copy's mark did ("(1)", "copy", "Copy of").
std::string base_of(const std::string& stem, bool* marked = nullptr, bool* copyish = nullptr);
// "ch3 final2.odt" -> {"ch3 final2", "odt"}; a name with no dot -> {name, ""}.
// Lower-cased extension; ".folio.gz" is one extension ("folio.gz").
void split_name(const std::string& name, std::string& stem, std::string& ext);
// "Tapper" -> "Tapper versions".
std::string versions_name(const std::string& base);
// Is `folder_name` a versions folder ("x versions", "versions")?
bool is_versions_folder(const std::string& folder_name);

struct Member {
    std::string   rel;               // relative to home
    std::uint64_t bytes = 0;
    std::int64_t  changed = 0;       // last change (a package: as the catalog has it)
    bool          package = false;
    std::uint64_t dev = 0, ino = 0;  // as the Look saw it -- re-checked at plan time
    bool          in_versions = false;   // already in the versions folder
    bool          chosen = false;        // your "keeper" decision is on it
    std::string   reflex;            // "" or why it looks like a reflex save
};

struct Family {
    std::string         folder;      // relative to home ("" = the top of home)
    std::string         base;        // as the keeper writes it ("ch3")
    std::string         ext;         // lower case ("odt"; "" = none)
    std::string         vdir;        // relative to home: folder + "/" + versions_name(base)
    std::vector<Member> members;     // newest first
    std::size_t         keeper = 0;  // index into members
    std::string         keeper_why;  // "the newest" / "you chose" / "the only one outside its versions folder"
    std::size_t         loose = 0;   // older saves NOT yet in the versions folder (0 = tidy)
    std::uint64_t       loose_bytes = 0;
    std::string key() const;         // folder + "/" + lower(base) + "." + ext -- names it in decisions
};

struct History {                     // app-managed history: recognised, left alone
    std::string   app;               // "Folio"
    std::string   what;              // "Tapper" (the project / jots folder), or "" for the whole place
    std::string   path;              // absolute folder
    std::size_t   count = 0;         // versions / days / recordings
    std::uint64_t bytes = 0;
    std::int64_t  oldest = 0, newest = 0;
    bool          in_home = false;   // the folder is under home and not hidden -- where Tidy looks
};
// Where Folio, jot and Trails keep their history, from their prefs (or their
// defaults) under `data_home` (~/.local/share) and `config_home` (~/.config).
// READ-ONLY; a place that isn't there is simply not listed.
std::vector<History> app_history(const std::string& home, const std::string& data_home,
                                 const std::string& config_home);

struct Report {
    bool                     ok = false;
    std::string              error;
    std::vector<Family>      families;     // with loose saves first (most loose bytes first), then the tidy ones by folder
    std::size_t              loose = 0;    // loose older saves, all families
    std::uint64_t            loose_bytes = 0;
    std::vector<History>     history;
    std::vector<std::string> notes;
};
// From the catalog in `data_dir` (a Look keeps it current) and the disk.
// Families you said are not one (decision "not-family" with detail = key())
// on every member are left out.
Report find(const std::string& data_dir, const std::string& home, const std::string& data_home,
            const std::string& config_home);

// Are these two files the same bytes? (Reads both; a package or a file over
// 256 MB is never compared -- false.)
bool same_bytes(const std::string& a, const std::string& b);

// The step for ONE family with `keeper` as its keeper: MakeDir the versions
// folder if it's missing; each older save outside it -> Move into it; the
// keeper, if it was inside, -> Move out beside it. Each is looked at again
// (same inode; a file's same mtime) -- changed, gone, or its name taken there
// -> left out and named in `dropped`. If the keeper can't come out, nothing
// is planned. previewed stays false: only the UI sets it.
struct Step {
    door::Plan               plan;
    std::vector<std::string> dropped;
    std::size_t              moves = 0;   // older saves moved in (not the keeper coming out)
    std::uint64_t            bytes = 0;
};
Step plan(const std::string& home, const Family& f, std::size_t keeper);
// Several families as ONE door step (one Undo), each as plan() would.
Step plan_all(const std::string& home, const std::vector<Family>& fs, const std::vector<std::size_t>& keepers);

// Decisions (Rouster's own catalog, not a user file):
//   "keeper"     on the chosen member (Keep this one instead);
//   "not-family" on every member, detail = key() -- never proposed again.
bool choose_keeper(const std::string& data_dir, const Family& f, std::size_t keeper, std::int64_t now);
bool not_a_family(const std::string& data_dir, const Family& f, std::int64_t now);

}  // namespace rouster::core::versions
