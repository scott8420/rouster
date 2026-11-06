#pragma once
#include "core/Door.hpp"
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
// Plan:   the light clean -- the checked build-output folders -> Trash
//         actions, unpreviewed. Retire / restore come with s007b.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::dev {

// ── git, read-only ──────────────────────────────────────────────────────────
struct Run {
    int         code = -1;        // exit status; -1 = did not run / killed
    std::string out, err;
    bool        timed_out = false;
};
// `git -C dir args...` with prompts off and optional locks off; killed after
// timeout_s or when *cancel turns true. The user's git config still applies
// (url rewrites, credential helpers), so ls-remote reaches what `git push` does.
Run git(const std::string& dir, const std::vector<std::string>& args, int timeout_s,
        const std::atomic<bool>* cancel = nullptr);

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
};

// READ-ONLY. abs: the repo's top folder.
Repo verify(const std::string& abs, const std::atomic<bool>* cancel = nullptr);

// ── lint ───────────────────────────────────────────────────────────────────
enum class Tier { Required, Naming, Unknown, Note };
const char* tier_name(Tier t);       // "missing", "naming", "not in the template", "note"
struct Drift {
    Tier        tier = Tier::Note;
    std::string what;                // one line
};
// The template, as lists (one place; the selftest reads them too).
const std::vector<std::string>& required_dirs();     // include src resources
const std::vector<std::string>& required_files();    // CMakeLists.txt build.sh
const std::vector<std::string>& optional_dirs();     // third_party libs data cmake tests
const std::vector<std::string>& known_files();       // install.sh uninstall.sh README.md LICENSE .gitignore
inline constexpr const char* kAppIdPrefix = "io.github.scott8420";

// abs: the workspace root; app: its app folder name.
std::vector<Drift> lint(const std::string& abs, const std::string& app);

// ── find ───────────────────────────────────────────────────────────────────
enum class Kind { Workspace, Repo };

struct BuildDir {
    std::string   rel;               // relative to home
    std::uint64_t bytes = 0, files = 0;
    bool          offered = true;    // false: shown, never planned (no remote)
    bool          checked = true;    // the pre-check: build output is regenerable
    std::string   note;              // why not offered, if it isn't
};

struct Project {
    Kind        kind = Kind::Repo;
    std::string rel;                 // workspace / repo root, relative to home
    std::string name;                // the app (workspace) or the folder name
    std::string app;                 // workspace: the app folder ("" for a repo)
    Repo        repo;
    std::vector<BuildDir> builds;
    std::vector<Drift>    drift;     // workspace only
    std::uint64_t fixes_files = 0;   // workspace: files under fixes/ (never touched)
};

struct Found {
    std::vector<Project>     projects;   // sorted by path
    std::vector<std::string> roots;      // the dev roots that existed, relative to home
    std::uint64_t errors = 0;
    bool          cancelled = false;
    std::string   refused;               // home unusable
};

// The dev roots looked for, relative to home.
const std::vector<std::string>& roots();

// abs: a folder. True and the app folder's name when it carries the signature.
bool is_workspace(const std::string& abs, std::string& app);

// READ-ONLY. Walks the roots, verifies each repo, lints workspaces, measures
// build output. Progress: dirs = folders walked, files = projects checked,
// bytes = build output measured.
Found find(const std::string& home, scan::Progress& progress);

// The checked + offered build folders as a door plan of Trash actions.
// previewed stays false -- only the UI, having shown the list, may set it.
door::Plan plan(const std::string& home, const std::vector<Project>& projects);

}  // namespace rouster::core::dev
