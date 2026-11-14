#pragma once
#include "core/Door.hpp"
#include "core/Git.hpp"
#include "core/Scan.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Dev projects (s007, REQUIREMENTS §4.5). FIND, VERIFY, LINT, PLAN -- all
// read-only. Nothing in this file changes the disk; the door acts.
//
// Find:   walks the dev roots (~/Develop, and ~/Projects, ~/src, ~/dev, ~/code
//         when they exist) a few levels deep. A folder with Scott's workspace
//         signature -- docs/RULES.md + scripts/*backup.sh + <app>/CMakeLists.txt
//         -- is a Workspace; any other folder holding .git is a Repo. Neither
//         is descended into.
//
// Verify: runs git in the repo, read-only (GIT_OPTIONAL_LOCKS=0, so even
//         `status` does not refresh the index; never fetch). All must pass to
//         count as "backed up":
//           committed      no uncommitted or untracked changes
//           pushed         no local commit missing from the remote, any branch
//           branches       no local-only branch
//           stashes        none
//           tags           every local tag on the remote, same object
//           live           `git ls-remote` answered -- the remote's heads NOW,
//                          never the cached refs/remotes
//         Ignored-but-present files are listed for a decision, not judged.
//         The remote that matters: the current branch's upstream remote, else
//         `origin`, else the first. No remote at all -> NoRemote: flagged
//         "not backed up" and never touched (its build output isn't offered).
//
// Lint:   a workspace against the template (§4.5): required, known-optional,
//         unknown; naming drift (backup script, .desktop.in place, app id).
//
// Clean:  (s020b, replaces the light clean and Retire) each project is put in
//         one group -- safe to clean (Backed up), no git (review), look into
//         (red) -- and gets up to three rows: Zips, Build output, Source. The
//         ticked, offered rows -> ONE plan of Trash actions, unpreviewed.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::dev {

// ── git (core/Git.hpp) ─────────────────────────────────────────────────────
using vcs::Run;
using vcs::git;

// "git@github.com:me/x.git" / "https://github.com/me/x" -> "github.com";
// a local path -> "this computer".
std::string url_host(const std::string& url);

// ── verify ─────────────────────────────────────────────────────────────────
enum class Status {
    Backed,        // every check passed against the live remote
    NotClean,      // something here is not on the remote (the checks say what)
    Unreachable,   // the remote did not answer: unknown, so treated as not clean
    NoRemote,      // a repo with no remote: not backed up, never touched
    NotRepo,       // a workspace whose app folder has no .git: same
};
const char* status_name(Status s);   // "Backed up", ...

struct Check {
    std::string name;      // "committed", "pushed", ...
    bool        ok = false;
    std::string detail;    // what failed, or what was found
};

struct Repo {
    Status      status = Status::NotRepo;
    std::string remote, url, host;   // the remote that matters
    std::string branch;              // "" = detached
    std::string head;                // full sha ("" = no commits)
    std::vector<std::string> other_remotes;
    std::vector<Check>       checks;
    std::vector<std::string> ignored;   // ignored-but-present, relative to the repo
    std::string error;               // why it couldn't be verified, if it couldn't
    bool        cleaned = false;     // s020b: every tracked file deleted here, nothing else
                                     //   changed -- Clean took the source; `git restore .`
};

// READ-ONLY. abs: the repo's top folder.
Repo verify(const std::string& abs, const std::atomic<bool>* cancel = nullptr);

// ── lint ───────────────────────────────────────────────────────────────────
enum class Tier { Required, Naming, Unknown, Note };
const char* tier_name(Tier t);       // "missing", "naming", "not in the template", "note"
struct Drift {
    Tier        tier = Tier::Note;
    std::string what;                // one line
    std::string name;                // Unknown: the entry's name, promotable; Note: a promoted
    bool        promoted = false;    //   name that is present (forgettable). "" otherwise.
};
// The template, as lists (one place; the selftest reads them too).
const std::vector<std::string>& required_dirs();     // include src resources
const std::vector<std::string>& required_files();    // CMakeLists.txt build.sh
const std::vector<std::string>& optional_dirs();     // third_party libs data cmake tests
const std::vector<std::string>& known_files();       // install.sh uninstall.sh README.md LICENSE .gitignore
inline constexpr const char* kAppIdPrefix = "io.github.scott8420";

// abs: the workspace root; app: its app folder name. known: names Scott has
// promoted to known-optional (folders or files, either level of the workspace).
std::vector<Drift> lint(const std::string& abs, const std::string& app,
                        const std::vector<std::string>& known = {});

// ── find ───────────────────────────────────────────────────────────────────
enum class Kind { Workspace, Repo };

// ── clean (s020b) ──────────────────────────────────────────────────────────
// Scott (2026-11-14): one view, every project sorted and colored.
enum class Group {
    Clean,      // ready: Backed up -- one checkbox takes Zips + Build output + Source
    NoGit,      // no repo, or a repo with no remote: not ready -- nothing offered
    LookInto,   // red: dirty, unpushed, unreachable -- nothing offered
};
const char* group_name(Group g);     // "Safe to clean", "No git -- review", "Look into"
Group group_of(const Repo& r);

enum class Part { Zips, Build, Source };
const char* part_name(Part p);       // "Zips", "Build output", "Source"

struct Item {
    std::string   rel;               // relative to home -- one Trash action
    std::uint64_t bytes = 0;
    std::string   why;               // the action's reason
};
struct Row {
    Part          part = Part::Build;
    std::vector<Item> items;
    std::uint64_t bytes = 0, files = 0;
    bool          offered = false;   // false: never planned, even ticked
    bool          checked = false;   // the pre-tick
    std::string   note;              // one line: what it is
    std::vector<std::string> stays;  // Source: ignored files left in place (not on the remote)
    std::vector<std::string> leaves; // Source: ignored files inside a folder that goes -- NOT on the remote
};

struct Project {
    Kind        kind = Kind::Repo;
    std::string rel;                 // workspace / repo root, relative to home
    std::string name;                // the app (workspace) or the folder name
    std::string app;                 // workspace: the app folder ("" for a repo)
    std::string repo_rel;            // the repo's folder, relative to home (workspace: <rel>/<app>)
    Repo        repo;
    Group       group = Group::NoGit;
    std::vector<Row>   rows;         // Zips, Build output, Source -- those that have anything
    std::vector<Drift> drift;        // workspace only
    std::uint64_t fixes_files = 0;   // workspace: files under fixes/ (never touched)
};

struct Found {
    std::vector<Project>     projects;   // sorted by path
    std::vector<std::string> roots;      // the dev roots that existed, relative to home
    std::uint64_t errors = 0;
    bool          cancelled = false;
    std::string   refused;               // home unusable
};

// ── push (s020a) ───────────────────────────────────────────────────────────
// Scott (2026-11-13): unpushed projects get pushed; dirty ones are reported,
// never committed or pushed -- he investigates. A push runs git, not the door:
// it changes no file of yours (only the remote, and git's own refs/remotes),
// and it can't be undone -- the preview says so. Never --force: a remote that
// moved on rejects the push, and that is reported.
//
// Dirty: anything uncommitted or untracked (the "committed" check failed).
bool dirty(const Repo& r);

struct PushPlan {
    std::string remote, host;
    std::vector<std::vector<std::string>> steps;   // git args, run in order in the repo
    std::vector<std::string> what;                 // one line per step, for the preview
    std::vector<std::string> stays;                // what a push can't carry (stashes)
    std::string why_not;                           // non-empty: not offered, and why
};
// Offered for a NotClean repo (not dirty, on a branch) whose
// pushed / branches / tags check failed and whose remote has nothing this
// copy hasn't fetched. Steps: `push --porcelain <remote> --all` (branches),
// `push --porcelain <remote> --tags` (tags).
PushPlan push_plan(const Project& p);

struct Pushed {
    bool ok = false;                 // every step's git exited 0
    std::vector<std::string> said;   // per step: "branches: ok" / the error's first line
    Repo after;                      // verify() again, against the live remote
};
// Runs the steps (120 s each, prompts off -- a push that needs a password
// fails and says so), then verifies again. abs: the repo's top folder.
Pushed push(const std::string& abs, const PushPlan& plan, const std::atomic<bool>* cancel = nullptr);

// The dev roots looked for, relative to home.
const std::vector<std::string>& roots();

// abs: a folder. True and the app folder's name when it carries the signature.
bool is_workspace(const std::string& abs, std::string& app);

// READ-ONLY. Walks the roots, verifies each repo, lints workspaces, measures
// build output. Progress: dirs = folders walked, files = projects checked,
// bytes = build output measured.
Found find(const std::string& home, scan::Progress& progress, const std::vector<std::string>& known = {});

// The row of a part, or nullptr.
const Row* row_of(const Project& p, Part part);
Row*       row_of(Project& p, Part part);

// Red's one line: why it needs looking into ("" when it doesn't).
std::string why_red(const Project& p);

// The ticked + offered rows as ONE door plan of Trash actions -- a Source
// folder that holds a build folder takes it along (the inner action is left
// out, never run twice). previewed stays false -- only the UI, having shown
// the preview, may set it. docs/, scripts/, fixes/ and .git never appear.
door::Plan clean_plan(const std::string& home, const std::vector<Project>& projects);

// The projects whose Source is ticked: the remote is asked once more for
// each before anything moves (the verdict on screen may be minutes old).
std::vector<const Project*> source_ticked(const std::vector<Project>& projects);

}  // namespace rouster::core::dev
