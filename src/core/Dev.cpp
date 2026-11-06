#include "core/Dev.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <ctime>
#include <functional>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

// Dev.cpp -- FIND, VERIFY, LINT, PLAN. Read-only: opendir/readdir/lstat, an
// ifstream for CMakeLists.txt, and git run as a child with optional locks off
// and nothing that writes a repo (no fetch, no gc, no index refresh). The
// source gate reads this file like every other.

namespace rouster::core::dev {

namespace {

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) if (!l.empty()) v.push_back(l);
    return v;
}
std::string first_line(const std::string& s) {
    const auto v = lines(s);
    return v.empty() ? std::string() : v.front();
}
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v;
    std::size_t p = 0;
    for (;;) {
        const auto q = s.find(c, p);
        v.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return v;
}
std::string listed(const std::vector<std::string>& v, std::size_t max = 3) {
    std::string s;
    for (std::size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
    if (v.size() > max) s += " and " + std::to_string(v.size() - max) + " more";
    return s;
}
std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

bool is_dir(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_file(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
}
std::vector<std::string> children(const std::string& p) {   // sorted names, no . / ..
    std::vector<std::string> v;
    DIR* d = ::opendir(p.c_str());
    if (!d) return v;
    while (dirent* e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..") v.push_back(n);
    }
    ::closedir(d);
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

std::string url_host(const std::string& url) {
    std::string u = url;
    if (u.empty()) return "";
    if (u.front() == '/' || u.starts_with("file://") || u.starts_with("./") || u.starts_with("../"))
        return "this computer";
    if (const auto p = u.find("://"); p != std::string::npos) {
        u = u.substr(p + 3);
        if (const auto at = u.find('@'); at != std::string::npos && at < u.find('/')) u = u.substr(at + 1);
        u = u.substr(0, u.find('/'));
        if (const auto c = u.find(':'); c != std::string::npos) u = u.substr(0, c);
        return u;
    }
    // scp-like: user@host:path
    if (const auto c = u.find(':'); c != std::string::npos) {
        u = u.substr(0, c);
        if (const auto at = u.find('@'); at != std::string::npos) u = u.substr(at + 1);
        return u;
    }
    return "this computer";
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Backed:      return "Backed up";
        case Status::NotClean:    return "Not clean";
        case Status::Unreachable: return "Can't reach remote";
        case Status::NoRemote:    return "No remote";
        case Status::NotRepo:     return "Not a repo";
        case Status::Retired:     return "Retired";
    }
    return "?";
}

// ── verify ─────────────────────────────────────────────────────────────────
namespace { constexpr int kLocal = 20, kLive = 30; }   // seconds: local git calls, ls-remote
Repo verify(const std::string& abs, const std::atomic<bool>* cancel) {

    Repo r;
    const auto g = [&](std::vector<std::string> a, int t = kLocal) { return git(abs, std::move(a), t, cancel); };

    const Run top = g({"rev-parse", "--show-toplevel"});
    if (top.code != 0) {
        r.status = Status::NotRepo;
        r.error = top.code == -1 ? "git did not run" : first_line(top.err);
        return r;
    }
    if (const Run h = g({"rev-parse", "-q", "--verify", "HEAD^{commit}"}); h.code == 0) r.head = trim(h.out);
    if (const Run b = g({"symbolic-ref", "-q", "--short", "HEAD"}); b.code == 0) r.branch = trim(b.out);

    // The remote that matters.
    const auto remotes = lines(g({"remote"}).out);
    if (remotes.empty()) {
        r.status = Status::NoRemote;
        r.error = "it has no remote, so it exists only on this computer";
        return r;
    }
    if (!r.branch.empty()) {
        const std::string up = trim(g({"config", "--get", "branch." + r.branch + ".remote"}).out);
        if (std::find(remotes.begin(), remotes.end(), up) != remotes.end()) r.remote = up;
    }
    if (r.remote.empty())
        r.remote = std::find(remotes.begin(), remotes.end(), "origin") != remotes.end() ? "origin" : remotes.front();
    for (const auto& x : remotes) if (x != r.remote) r.other_remotes.push_back(x);
    r.url  = trim(g({"remote", "get-url", r.remote}).out);
    r.host = url_host(r.url);

    // committed
    {
        const auto st = lines(g({"status", "--porcelain=v1", "--untracked-files=all"}).out);
        std::vector<std::string> names;
        for (const auto& l : st) names.push_back(l.size() > 3 ? l.substr(3) : l);
        r.checks.push_back({"committed", st.empty(),
                            st.empty() ? "nothing uncommitted or untracked"
                                       : plural(st.size(), "change", "changes") + " not committed: " + listed(names)});
    }
    // stashes
    {
        const auto s = lines(g({"stash", "list"}).out);
        r.checks.push_back({"stashes", s.empty(), s.empty() ? "no stashes" : plural(s.size(), "stash", "stashes") + " (stashes are never pushed)"});
    }
    // ignored-but-present: listed, not judged
    r.ignored = lines(g({"ls-files", "--others", "--ignored", "--exclude-standard", "--directory"}).out);

    if (r.head.empty()) {
        r.checks.push_back({"pushed", false, "no commits yet"});
        r.status = Status::NotClean;
        return r;
    }

    // live: the remote's refs now
    const Run lr = g({"ls-remote", r.remote}, kLive);
    if (lr.code != 0) {
        const std::string why = lr.timed_out ? "no answer in " + std::to_string(kLive) + " s"
                              : (cancel && cancel->load()) ? "stopped"
                              : first_line(lr.err).empty() ? "git ls-remote failed" : first_line(lr.err);
        r.checks.push_back({"live", false, "could not reach " + r.remote + " (" + r.host + "): " + why});
        r.error = why;
        r.status = Status::Unreachable;
        return r;
    }
    std::map<std::string, std::string> rheads, rtags;
    for (const auto& l : lines(lr.out)) {
        const auto t = l.find('\t');
        if (t == std::string::npos) continue;
        const std::string sha = l.substr(0, t), ref = l.substr(t + 1);
        if (ref.starts_with("refs/heads/")) rheads[ref.substr(11)] = sha;
        else if (ref.starts_with("refs/tags/") && !ref.ends_with("^{}")) rtags[ref.substr(10)] = sha;
    }
    r.checks.push_back({"live", true, r.remote + " answered (" + r.host + ") -- " + plural(rheads.size(), "branch", "branches")});

    const auto known = [&](const std::string& sha) { return g({"cat-file", "-e", sha + "^{commit}"}).code == 0; };
    const auto contains = [&](const std::string& remote_sha, const std::string& local_sha) {
        return remote_sha == local_sha || g({"merge-base", "--is-ancestor", local_sha, remote_sha}).code == 0;
    };

    // pushed + branches
    std::vector<std::string> unpushed, local_only, unknown;
    const auto refs = lines(g({"for-each-ref", "--format=%(refname:short)%09%(objectname)%09%(upstream:remotename)%09%(upstream:remoteref)",
                               "refs/heads"}).out);
    for (const auto& l : refs) {
        const auto f = split(l, '\t');
        if (f.size() < 4) continue;
        const std::string& name = f[0];
        const std::string& sha = f[1];
        std::string target = name;
        if (f[2] == r.remote && f[3].starts_with("refs/heads/")) target = f[3].substr(11);
        const auto it = rheads.find(target);
        if (it == rheads.end()) { local_only.push_back(name); continue; }
        if (it->second == sha) continue;
        if (!known(it->second)) { unknown.push_back(name); continue; }
        if (!contains(it->second, sha)) {
            const std::string n = trim(g({"rev-list", "--count", it->second + ".." + sha}).out);
            unpushed.push_back(name + " (" + n + " ahead)");
        }
    }
    if (r.branch.empty()) {   // detached: HEAD must be inside some remote branch
        bool inside = false;
        for (const auto& [n, sha] : rheads)
            if (known(sha) && contains(sha, r.head)) { inside = true; break; }
        if (!inside) unpushed.push_back("detached HEAD " + r.head.substr(0, 7));
    }
    {
        std::string d;
        if (!unpushed.empty()) d = "not on " + r.remote + ": " + listed(unpushed);
        if (!unknown.empty()) d += std::string(d.empty() ? "" : "; ") + r.remote + " has commits not fetched here on " +
                                   listed(unknown) + " -- fetch, then look again";
        r.checks.push_back({"pushed", unpushed.empty() && unknown.empty(),
                            d.empty() ? "every branch's commits are on " + r.remote : d});
    }
    r.checks.push_back({"branches", local_only.empty(),
                        local_only.empty() ? "no local-only branches"
                                           : "only here: " + listed(local_only)});
    // tags
    {
        std::vector<std::string> missing;
        std::size_t n = 0;
        for (const auto& l : lines(g({"for-each-ref", "--format=%(refname:short)%09%(objectname)", "refs/tags"}).out)) {
            const auto f = split(l, '\t');
            if (f.size() < 2) continue;
            ++n;
            const auto it = rtags.find(f[0]);
            if (it == rtags.end() || it->second != f[1]) missing.push_back(f[0]);
        }
        r.checks.push_back({"tags", missing.empty(),
                            missing.empty() ? (n ? "all " + plural(n, "tag", "tags") + " pushed" : "no tags")
                                            : "not on " + r.remote + ": " + listed(missing)});
    }

    bool all = true;
    for (const auto& c : r.checks) all = all && c.ok;
    r.status = all ? Status::Backed : Status::NotClean;
    return r;
}

// ── lint ───────────────────────────────────────────────────────────────────
const char* tier_name(Tier t) {
    switch (t) {
        case Tier::Required: return "missing";
        case Tier::Naming:   return "naming";
        case Tier::Unknown:  return "not in the template";
        case Tier::Note:     return "note";
    }
    return "?";
}
const std::vector<std::string>& required_dirs()  { static const std::vector<std::string> v = {"include", "src", "resources"}; return v; }
const std::vector<std::string>& required_files() { static const std::vector<std::string> v = {"CMakeLists.txt", "build.sh"}; return v; }
const std::vector<std::string>& optional_dirs()  { static const std::vector<std::string> v = {"third_party", "libs", "data", "cmake", "tests"}; return v; }
const std::vector<std::string>& known_files()    { static const std::vector<std::string> v = {"install.sh", "uninstall.sh", "README.md", "LICENSE", ".gitignore"}; return v; }

namespace {
bool in(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::uint64_t count_files(const std::string& abs, int depth = 0) {
    if (depth > 12) return 0;
    std::uint64_t n = 0;
    for (const auto& c : children(abs)) {
        struct stat st{};
        const std::string p = abs + "/" + c;
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) n += count_files(p, depth + 1);
        else ++n;
    }
    return n;
}
}  // namespace

std::vector<Drift> lint(const std::string& abs, const std::string& app, const std::vector<std::string>& known) {
    std::vector<Drift> v;
    const auto promoted = [&](const std::string& rel, const std::string& name, bool dir) {
        if (!in(known, name)) return false;
        v.push_back({Tier::Note, rel + (dir ? "/" : "") + " -- known-optional (you added it)", name, true});
        return true;
    };
    const std::string a = abs + "/" + app;
    for (const auto& d : required_dirs())
        if (!is_dir(a + "/" + d)) v.push_back({Tier::Required, app + "/" + d + "/"});
    for (const auto& f : required_files())
        if (!is_file(a + "/" + f)) v.push_back({Tier::Required, app + "/" + f});

    // The app folder's top: anything the template doesn't know. Hidden entries
    // (.git, .clang-format, editor folders) and build/ are left out.
    for (const auto& c : children(a)) {
        if (c.starts_with(".") || c == "build") continue;
        const bool dir = is_dir(a + "/" + c);
        if (dir && (in(required_dirs(), c) || in(optional_dirs(), c))) continue;
        if (!dir && (in(required_files(), c) || in(known_files(), c) || c.ends_with(".code-workspace"))) continue;
        if (promoted(app + "/" + c, c, dir)) continue;
        v.push_back({Tier::Unknown, app + "/" + c + (dir ? "/" : ""), c});
    }
    // The workspace's top.
    for (const auto& c : children(abs)) {
        if (c.starts_with(".") || c == app) continue;
        if (c == "docs" || c == "scripts" || c == "backups" || c == "fixes" || c.ends_with(".code-workspace")) continue;
        if (c == kRestoreFile) continue;
        const bool dir = is_dir(abs + "/" + c);
        if (promoted(c, c, dir)) continue;
        v.push_back({Tier::Unknown, c + (dir ? "/" : ""), c});
    }

    // Naming drift.
    if (!is_file(abs + "/scripts/" + app + "_backup.sh")) {
        std::string other;
        for (const auto& c : children(abs + "/scripts"))
            if (c.ends_with("backup.sh")) { other = c; break; }
        v.push_back({Tier::Naming, "scripts/" + (other.empty() ? std::string("(no backup script)") : other) +
                                       " -- the template names it " + app + "_backup.sh"});
    }
    bool desktop_in_data = false;
    for (const auto& c : children(a + "/data")) if (c.ends_with(".desktop.in")) desktop_in_data = true;
    for (const auto& c : children(a + "/resources"))
        if (c.ends_with(".desktop.in"))
            v.push_back({Tier::Naming, app + "/resources/" + c + " -- the template keeps the .desktop.in in data/"});
    if (!desktop_in_data) {
        bool any = false;
        for (const auto& c : children(a + "/resources")) if (c.ends_with(".desktop.in")) any = true;
        if (!any) v.push_back({Tier::Naming, app + "/data/ has no .desktop.in"});
    }
    // App id: a placeholder id left in CMakeLists.txt or a data file's name.
    {
        std::ifstream in_cm(a + "/CMakeLists.txt");
        std::stringstream ss;
        ss << in_cm.rdbuf();
        const std::string cm = ss.str();
        std::string found;
        if (const auto p = cm.find("io.github."); p != std::string::npos) {
            auto e = p;
            while (e < cm.size() && (std::isalnum(static_cast<unsigned char>(cm[e])) || cm[e] == '.' || cm[e] == '_' || cm[e] == '-')) ++e;
            found = cm.substr(p, e - p);
        }
        for (const auto& c : children(a + "/data"))
            if (found.empty() && c.starts_with("io.github.")) found = c;
        if (!found.empty() && !found.starts_with(std::string(kAppIdPrefix) + "."))
            v.push_back({Tier::Naming, "app id " + found.substr(0, found.find(".desktop")) + " -- the template uses " +
                                           kAppIdPrefix + ".<App>"});
    }
    // fixes/: never touched, reported when there's something in it.
    if (is_dir(abs + "/fixes"))
        if (const auto n = count_files(abs + "/fixes"))
            v.push_back({Tier::Note, "fixes/ holds " + plural(n, "file", "files") + " -- Rouster never touches it"});
    return v;
}

// ── find ───────────────────────────────────────────────────────────────────
const std::vector<std::string>& roots() {
    static const std::vector<std::string> v = {"Develop", "Projects", "src", "dev", "code"};
    return v;
}

bool is_workspace(const std::string& abs, std::string& app) {
    if (!is_file(abs + "/docs/RULES.md")) return false;
    std::vector<std::string> scripts;
    for (const auto& c : children(abs + "/scripts"))
        if (c.ends_with("backup.sh") && is_file(abs + "/scripts/" + c)) scripts.push_back(c);
    if (scripts.empty()) return false;
    std::vector<std::string> apps;
    for (const auto& c : children(abs)) {
        if (c.starts_with(".") || c == "docs" || c == "scripts" || c == "backups" || c == "fixes") continue;
        if (is_dir(abs + "/" + c) && is_file(abs + "/" + c + "/CMakeLists.txt")) apps.push_back(c);
    }
    if (apps.empty()) return false;
    app = apps.front();
    for (const auto& x : apps)   // the one the backup script is named for wins
        if (in(scripts, x + "_backup.sh")) { app = x; break; }
    return true;
}

namespace {

// du, one filesystem, no symlink followed.
void measure(const std::string& abs, dev_t dev, int depth, BuildDir& b, scan::Progress& prog) {
    if (depth > 24 || prog.cancel.load(std::memory_order_relaxed)) return;
    for (const auto& c : children(abs)) {
        struct stat st{};
        const std::string p = abs + "/" + c;
        if (::lstat(p.c_str(), &st) != 0 || st.st_dev != dev) continue;
        const auto use = static_cast<std::uint64_t>(st.st_blocks) * 512u;
        b.bytes += use;
        prog.bytes.fetch_add(use, std::memory_order_relaxed);
        if (S_ISDIR(st.st_mode)) measure(p, dev, depth + 1, b, prog);
        else ++b.files;
    }
}

// A build-output candidate: a real folder (not a symlink), on the project's
// filesystem, with nothing in it tracked by git.
void consider(const std::string& home, const std::string& repo_abs, const std::string& abs, dev_t dev,
              Project& p, scan::Progress& prog) {
    struct stat st{};
    if (::lstat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != dev) return;
    if (!repo_abs.empty() && p.repo.status != Status::NotRepo) {
        const std::string rel_in_repo = abs.substr(repo_abs.size() + 1);
        const Run t = git(repo_abs, {"ls-files", "--", rel_in_repo}, 20, &prog.cancel);
        if (t.code != 0 || !trim(t.out).empty()) return;   // tracked (or unknown): not build output
    }
    BuildDir b;
    b.rel = abs.substr(home.size() + 1);
    measure(abs, dev, 0, b, prog);
    b.bytes += static_cast<std::uint64_t>(st.st_blocks) * 512u;
    if (p.repo.status == Status::NoRemote || p.repo.status == Status::NotRepo) {
        b.offered = b.checked = false;
        b.note = "not backed up -- left alone";
    }
    p.builds.push_back(std::move(b));
}

void builds_of(const std::string& home, Project& p, scan::Progress& prog) {
    const std::string root = home + "/" + p.rel;
    struct stat st{};
    if (::lstat(root.c_str(), &st) != 0) return;
    if (p.kind == Kind::Workspace) {
        const std::string a = root + "/" + p.app;
        consider(home, a, a + "/build", st.st_dev, p, prog);
        for (const auto& lib : children(a + "/libs"))
            consider(home, a, a + "/libs/" + lib + "/build", st.st_dev, p, prog);
    } else {
        for (const auto& c : children(root))
            if (c == "build" || c.starts_with("cmake-build-"))
                consider(home, root, root + "/" + c, st.st_dev, p, prog);
    }
}

void walk(const std::string& home, const std::string& rel, dev_t dev, int depth, Found& f, scan::Progress& prog,
          const std::vector<std::string>& known) {
    if (depth > 4 || prog.cancel.load()) return;
    const std::string abs = home + "/" + rel;
    prog.dirs.fetch_add(1, std::memory_order_relaxed);

    std::string app;
    if (RestoreInfo ri; is_retired(abs, ri)) {
        Project p;
        p.kind = Kind::Workspace;
        p.rel = rel;
        p.app = p.name = ri.app;
        p.retired = true;
        p.restore = ri;
        p.repo.status = Status::Retired;
        p.repo.url = ri.url;
        p.repo.host = url_host(ri.url);
        p.repo.branch = ri.branch;
        p.repo.head = ri.commit;
        if (is_dir(abs + "/fixes"))
            if (const auto n = count_files(abs + "/fixes")) {
                p.fixes_files = n;
                p.drift.push_back({Tier::Note, "fixes/ holds " + plural(n, "file", "files") + " -- Rouster never touches it"});
            }
        f.projects.push_back(std::move(p));
        prog.files.fetch_add(1);
        return;
    }
    if (is_workspace(abs, app)) {
        Project p;
        p.kind = Kind::Workspace;
        p.rel = rel;
        p.app = app;
        p.name = app;
        const std::string a = abs + "/" + app;
        if (exists(a + "/.git")) p.repo = verify(a, &prog.cancel);
        else { p.repo.status = Status::NotRepo; p.repo.error = app + "/ has no .git"; }
        p.drift = lint(abs, app, known);
        for (const auto& d : p.drift)
            if (d.tier == Tier::Note && d.what.starts_with("fixes/")) p.fixes_files = count_files(abs + "/fixes");
        builds_of(home, p, prog);
        f.projects.push_back(std::move(p));
        prog.files.fetch_add(1);
        return;
    }
    if (exists(abs + "/.git")) {
        Project p;
        p.kind = Kind::Repo;
        p.rel = rel;
        p.name = rel.substr(rel.rfind('/') + 1);
        p.repo = verify(abs, &prog.cancel);
        builds_of(home, p, prog);
        f.projects.push_back(std::move(p));
        prog.files.fetch_add(1);
        return;
    }
    for (const auto& c : children(abs)) {
        if (c.starts_with(".") || c == "build" || c == "node_modules" || c == "fixes" || c.starts_with("cmake-build-"))
            continue;
        struct stat st{};
        const std::string p = abs + "/" + c;
        if (::lstat(p.c_str(), &st) != 0) { ++f.errors; continue; }
        if (!S_ISDIR(st.st_mode) || st.st_dev != dev) continue;   // no symlinks, one filesystem
        walk(home, rel + "/" + c, dev, depth + 1, f, prog, known);
    }
}

}  // namespace

Found find(const std::string& home, scan::Progress& prog, const std::vector<std::string>& known) {
    Found f;
    struct stat hs{};
    if (home.empty() || home == "/" || ::lstat(home.c_str(), &hs) != 0 || !S_ISDIR(hs.st_mode)) {
        f.refused = "home folder '" + home + "' is not usable";
        prog.done = true;
        return f;
    }
    for (const auto& r : roots()) {
        struct stat st{};
        const std::string abs = home + "/" + r;
        if (::lstat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;   // a symlinked root is not followed
        f.roots.push_back(r);
        walk(home, r, st.st_dev, 0, f, prog, known);
    }
    std::sort(f.projects.begin(), f.projects.end(), [](const Project& a, const Project& b) { return a.rel < b.rel; });
    f.cancelled = prog.cancel.load();
    prog.done = true;
    return f;
}

door::Plan plan(const std::string& home, const std::vector<Project>& projects) {
    door::Plan p;
    std::size_t n = 0;
    for (const auto& pr : projects) {
        bool any = false;
        for (const auto& b : pr.builds) {
            if (!b.offered || !b.checked) continue;
            door::Action a;
            a.verb = door::Verb::Trash;
            a.from = home + "/" + b.rel;
            a.reason = "build output of " + pr.name + " -- the next build makes it again";
            a.bytes = b.bytes;
            p.actions.push_back(std::move(a));
            any = true;
        }
        if (any) ++n;
    }
    p.label = "Dev projects: build output of " + plural(n, "project", "projects");
    return p;
}

// ── retire / restore (s007b) ────────────────────────────────────────────────
std::string restore_text(const RestoreInfo& r) {
    std::string s;
    s += "Rouster retired this project's working copy on " + r.retired + ".\n";
    s += "Everything in it is on its remote; Rouster checked before it moved anything.\n";
    s += "The working copy went to the Trash. To get it back: Rouster -> Dev projects ->\n";
    s += "Restore (one click), or by hand, in this folder:\n\n";
    s += "  git clone " + r.url + " " + r.app + (r.branch.empty() ? "" : " && git -C " + r.app + " checkout " + r.branch) + "\n\n";
    s += "app: " + r.app + "\n";
    s += "url: " + r.url + "\n";
    s += "branch: " + r.branch + "\n";
    s += "commit: " + r.commit + "\n";
    s += "retired: " + r.retired + "\n";
    return s;
}

bool parse_restore(const std::string& text, RestoreInfo& out) {
    RestoreInfo r;
    for (const auto& l : lines(text)) {
        const auto c = l.find(": ");
        if (c == std::string::npos || l.starts_with(" ")) continue;
        const std::string k = l.substr(0, c), v = trim(l.substr(c + 2));
        if (k == "app") r.app = v;
        else if (k == "url") r.url = v;
        else if (k == "branch") r.branch = v;
        else if (k == "commit") r.commit = v;
        else if (k == "retired") r.retired = v;
    }
    if (r.app.empty() || r.url.empty() || r.commit.empty() || r.app.find('/') != std::string::npos ||
        r.app.starts_with("."))
        return false;
    out = r;
    return true;
}

bool is_retired(const std::string& abs, RestoreInfo& out) {
    if (!is_file(abs + "/docs/RULES.md") || !is_file(abs + "/" + kRestoreFile)) return false;
    bool script = false;
    for (const auto& c : children(abs + "/scripts")) script = script || c.ends_with("backup.sh");
    if (!script) return false;
    std::ifstream in(abs + "/" + kRestoreFile);
    std::stringstream ss;
    ss << in.rdbuf();
    RestoreInfo r;
    if (!parse_restore(ss.str(), r)) return false;
    if (exists(abs + "/" + r.app)) return false;   // the app is back: a workspace again
    out = r;
    return true;
}

door::Plan retire_plan(const std::string& home, const Project& p, std::int64_t now, std::string& why_not,
                       RetireView* view) {
    door::Plan plan;
    const std::string ws = home + "/" + p.rel;
    if (p.kind != Kind::Workspace || p.retired) { why_not = "only a workspace can be retired"; return plan; }
    if (p.repo.status != Status::Backed) {
        why_not = std::string("it is not backed up (") + status_name(p.repo.status) + ") -- nothing is retired until it is";
        return plan;
    }
    if (!is_dir(ws + "/" + p.app)) { why_not = p.app + "/ is not there"; return plan; }
    if (exists(ws + "/" + kRestoreFile)) { why_not = std::string(kRestoreFile) + " is already there"; return plan; }

    RestoreInfo ri;
    ri.app = p.app;
    ri.url = p.repo.url;
    ri.branch = p.repo.branch;
    ri.commit = p.repo.head;
    {
        std::time_t t = static_cast<std::time_t>(now);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
        ri.retired = buf;
    }
    door::Action w;
    w.verb = door::Verb::Write;
    w.to = ws + "/" + kRestoreFile;
    w.content = restore_text(ri);
    w.reason = "how to get " + p.app + " back: " + ri.url + " at " + ri.commit.substr(0, 7);
    w.bytes = w.content.size();
    plan.actions.push_back(std::move(w));

    door::Action wc;
    wc.verb = door::Verb::Trash;
    wc.from = ws + "/" + p.app;
    wc.reason = "the working copy -- all of it is on " + p.repo.host;
    {
        scan::Progress pg;
        BuildDir b;
        struct stat st{};
        if (::lstat(wc.from.c_str(), &st) == 0) measure(wc.from, st.st_dev, 0, b, pg);
        wc.bytes = b.bytes;
    }
    plan.actions.push_back(std::move(wc));

    for (const auto& c : children(ws + "/backups/Old")) {
        door::Action a;
        a.verb = door::Verb::Trash;
        a.from = ws + "/backups/Old/" + c;
        a.reason = "an old session backup -- the repo is verified clean";
        struct stat st{};
        if (::lstat(a.from.c_str(), &st) == 0) a.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
        plan.actions.push_back(std::move(a));
    }
    // backups/: keep the newest zip (Q1), the rest go.
    std::vector<std::pair<std::int64_t, std::string>> zips;
    for (const auto& c : children(ws + "/backups")) {
        struct stat st{};
        const std::string z = ws + "/backups/" + c;
        if (c.ends_with(".zip") && ::lstat(z.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            zips.emplace_back(static_cast<std::int64_t>(st.st_mtim.tv_sec), c);
    }
    std::sort(zips.begin(), zips.end(), std::greater<>());
    for (std::size_t i = 1; i < zips.size(); ++i) {
        door::Action a;
        a.verb = door::Verb::Trash;
        a.from = ws + "/backups/" + zips[i].second;
        a.reason = "an older session zip -- the newest one stays";
        struct stat st{};
        if (::lstat(a.from.c_str(), &st) == 0) a.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
        plan.actions.push_back(std::move(a));
    }
    if (view) {
        view->keep.clear();
        for (const auto& c : children(ws)) {
            if (c == p.app || c == "backups") continue;
            view->keep.push_back(c + (is_dir(ws + "/" + c) ? "/" : ""));
        }
        if (!zips.empty()) view->keep.push_back("backups/" + zips.front().second);
        view->keep.push_back(std::string(kRestoreFile) + " (new)");
        view->ignored.clear();
        for (const auto& i : p.repo.ignored) {
            const std::string n = i.ends_with("/") ? i.substr(0, i.size() - 1) : i;
            const std::string last = n.substr(n.rfind('/') + 1);
            if (last == "build" || last.starts_with("cmake-build-")) continue;
            view->ignored.push_back(i);
        }
    }
    plan.label = "Retire " + p.name;
    return plan;
}

door::Plan restore_plan(const std::string& home, const Project& p, std::string& why_not) {
    door::Plan plan;
    if (!p.retired) { why_not = "it is not retired"; return plan; }
    const std::string ws = home + "/" + p.rel;
    door::Action c;
    c.verb = door::Verb::Clone;
    c.to = ws + "/" + p.restore.app;
    c.url = p.restore.url;
    c.branch = p.restore.branch;
    c.commit = p.restore.commit;
    c.reason = "clone " + p.restore.url + (p.restore.branch.empty() ? "" : " (" + p.restore.branch + ")") +
               " -- it must contain " + p.restore.commit.substr(0, 7);
    plan.actions.push_back(std::move(c));
    door::Action t;
    t.verb = door::Verb::Trash;
    t.from = ws + "/" + kRestoreFile;
    t.reason = "no longer needed once the working copy is back";
    plan.actions.push_back(std::move(t));
    plan.label = "Restore " + p.name;
    return plan;
}

}  // namespace rouster::core::dev
