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
bool is_elf(const std::string& p) {   // a program built here (ELF magic)
    std::ifstream in(p, std::ios::binary);
    char m[4] = {};
    return in.read(m, 4) && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
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
        bool only_deleted = !st.empty();
        for (const auto& l : st) {
            names.push_back(l.size() > 3 ? l.substr(3) : l);
            only_deleted = only_deleted && l.starts_with(" D ");
        }
        // s020b: Clean took the source -- EVERY tracked file deleted in the
        // working tree (none staged), nothing else changed. Not a change of
        // yours: the commit holds it all, `git restore .` brings it back.
        // .gitignore files stay (Clean never takes them -- they keep ignored files ignored).
        if (only_deleted) {
            std::size_t kept = 0, n = 0;
            for (const auto& t : lines(g({"ls-files"}).out)) {
                ++n;
                kept += t == ".gitignore" || t.ends_with("/.gitignore");
            }
            r.cleaned = n - kept == st.size();
        }
        r.checks.push_back({"committed", st.empty() || r.cleaned,
                            st.empty()  ? "nothing uncommitted or untracked"
                            : r.cleaned ? "source cleaned -- every file is in the last commit; git restore . brings it back"
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
bool dirty(const Repo& r) {
    for (const auto& c : r.checks) if (c.name == "committed") return !c.ok;
    return false;
}

PushPlan push_plan(const Project& p) {
    PushPlan pl;
    const Repo& r = p.repo;
    pl.remote = r.remote;
    pl.host = r.host;
    const auto check = [&](const char* n) -> const Check* {
        for (const auto& c : r.checks) if (c.name == n) return &c;
        return nullptr;
    };
    if (r.status == Status::Backed) { pl.why_not = "already on " + r.remote; return pl; }
    if (r.status != Status::NotClean) { pl.why_not = std::string(status_name(r.status)) + (r.error.empty() ? "" : ": " + r.error); return pl; }
    if (dirty(r)) { pl.why_not = "dirty -- uncommitted or untracked changes; look at it yourself"; return pl; }
    if (r.branch.empty()) { pl.why_not = "detached HEAD -- check out a branch first"; return pl; }
    if (r.head.empty()) { pl.why_not = "no commits yet"; return pl; }
    const Check* pushed = check("pushed");
    const Check* branches = check("branches");
    const Check* tags = check("tags");
    if (!pushed || !branches || !tags) { pl.why_not = "the remote wasn't reached"; return pl; }
    if (pushed->detail.find("not fetched here") != std::string::npos) {
        pl.why_not = r.remote + " has commits this copy hasn't fetched -- fetch and merge first";
        return pl;
    }
    if (!pushed->ok || !branches->ok) {
        pl.steps.push_back({"push", "--porcelain", r.remote, "--all"});
        pl.what.push_back("Push every branch to " + r.remote + " (" + r.host + ")" +
                          (pushed->ok ? "" : " -- " + pushed->detail) + (branches->ok ? "" : "; " + branches->detail));
    }
    if (!tags->ok) {
        pl.steps.push_back({"push", "--porcelain", r.remote, "--tags"});
        pl.what.push_back("Push the tags -- " + tags->detail);
    }
    if (const Check* st = check("stashes"); st && !st->ok) pl.stays.push_back(st->detail + " -- they stay here");
    if (pl.steps.empty()) pl.why_not = "nothing to push";
    return pl;
}

Pushed push(const std::string& abs, const PushPlan& plan, const std::atomic<bool>* cancel) {
    Pushed out;
    out.ok = !plan.steps.empty() && plan.why_not.empty();
    for (const auto& s : plan.steps) {
        if (cancel && cancel->load()) { out.ok = false; out.said.push_back("stopped"); break; }
        const Run r = git(abs, s, 120, cancel);
        const std::string what = s.back() == "--tags" ? "tags" : "branches";
        if (r.code == 0) out.said.push_back(what + ": pushed");
        else {
            out.ok = false;
            std::string why;
            for (const auto& l : lines(r.out))   // porcelain: "!\t<from>:<to>\t[rejected] (fetch first)"
                if (l.starts_with("!")) {
                    const auto f = split(l, '\t');
                    std::string ref = f.size() > 1 ? f[1].substr(f[1].find(':') + 1) : "";
                    if (ref.starts_with("refs/heads/")) ref = ref.substr(11);
                    else if (ref.starts_with("refs/tags/")) ref = "tag " + ref.substr(10);
                    why = (ref.empty() ? "" : ref + ": ") + (f.size() > 2 ? f[2] : l);
                    break;
                }
            if (r.timed_out) why = "no answer in 120 s";
            else if (why.empty()) why = first_line(r.err);
            out.said.push_back(what + ": " + (why.empty() ? "git push failed" : why));
        }
    }
    out.after = verify(abs, cancel);
    return out;
}

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
        if (!dir && is_elf(a + "/" + c)) continue;   // a program built here: build output, not the template's
        if (dir && (in(required_dirs(), c) || in(optional_dirs(), c))) continue;
        if (!dir && (in(required_files(), c) || in(known_files(), c) || c.ends_with(".code-workspace"))) continue;
        if (promoted(app + "/" + c, c, dir)) continue;
        v.push_back({Tier::Unknown, app + "/" + c + (dir ? "/" : ""), c});
    }
    // The workspace's top.
    for (const auto& c : children(abs)) {
        if (c.starts_with(".") || c == app) continue;
        if (c == "docs" || c == "scripts" || c == "backups" || c == "fixes" || c.ends_with(".code-workspace")) continue;
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
        if (!is_dir(abs + "/" + c)) continue;
        if (is_file(abs + "/" + c + "/CMakeLists.txt")) apps.push_back(c);
        // s020b: its source cleaned -- only .git left, and git still tracks a CMakeLists.txt.
        else if (exists(abs + "/" + c + "/.git") &&
                 !trim(git(abs + "/" + c, {"ls-files", "--", "CMakeLists.txt"}, 20).out).empty())
            apps.push_back(c);
    }
    if (apps.empty()) return false;
    app = apps.front();
    for (const auto& x : apps)   // the one the backup script is named for wins
        if (in(scripts, x + "_backup.sh")) { app = x; break; }
    return true;
}

namespace {

struct BuildDir_ { std::uint64_t bytes = 0, files = 0; };

// du, one filesystem, no symlink followed.
void measure(const std::string& abs, dev_t dev, int depth, BuildDir_& b, scan::Progress& prog) {
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


std::uint64_t size_of(const std::string& abs, scan::Progress& prog, std::uint64_t* files = nullptr) {   // du, one fs
    struct stat st{};
    if (::lstat(abs.c_str(), &st) != 0) return 0;
    BuildDir_ b;
    b.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
    if (S_ISDIR(st.st_mode)) measure(abs, st.st_dev, 0, b, prog);
    else b.files = 1;
    if (files) *files += b.files;
    return b.bytes;
}

// A build-output folder: a real folder (not a symlink), on the project's
// filesystem, with nothing in it tracked by git.
bool build_dir(const std::string& repo_abs, const std::string& abs, dev_t dev, const Project& p, scan::Progress& prog) {
    struct stat st{};
    if (::lstat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != dev) return false;
    if (p.repo.status != Status::NotRepo) {
        const std::string rel_in_repo = abs.substr(repo_abs.size() + 1);
        const Run t = git(repo_abs, {"ls-files", "--", rel_in_repo}, 20, &prog.cancel);
        if (t.code != 0 || !trim(t.out).empty()) return false;   // tracked (or unknown): not build output
    }
    return true;
}

void add(Row& row, const std::string& home, const std::string& abs, std::string why, scan::Progress& prog) {
    Item it;
    it.rel = abs.substr(home.size() + 1);
    it.bytes = size_of(abs, prog, &row.files);
    it.why = std::move(why);
    row.bytes += it.bytes;
    row.items.push_back(std::move(it));
}

void rows_of(const std::string& home, Project& p, scan::Progress& prog) {
    const std::string root = home + "/" + p.rel;
    const std::string repo = home + "/" + p.repo_rel;
    struct stat st{};
    if (::lstat(root.c_str(), &st) != 0) return;
    // s020c (Scott: "is it ready or not, with only one checkbox if it is"):
    // only a ready project (Backed up) is offered, and nothing starts ticked.
    const bool clean = p.group == Group::Clean;
    const auto finish = [&](Row& r, bool) {
        r.offered = clean;
        r.checked = false;
        if (!r.items.empty()) p.rows.push_back(std::move(r));
    };

    // Zips: every zip in backups/ and everything in backups/Old/ (workspaces).
    if (p.kind == Kind::Workspace) {
        Row z;
        z.part = Part::Zips;
        for (const auto& c : children(root + "/backups"))
            if (c.ends_with(".zip") && is_file(root + "/backups/" + c))
                add(z, home, root + "/backups/" + c, "a session zip of " + p.name, prog);
        for (const auto& c : children(root + "/backups/Old"))
            add(z, home, root + "/backups/Old/" + c, "an old session backup of " + p.name + " (backups/Old)", prog);
        z.note = plural(z.items.size(), "item", "items") + " in backups/ and backups/Old/";
        finish(z, true);
    }

    // Build output: build/ folders, and binaries built here that git ignores.
    std::vector<std::string> builds;   // relative to the repo
    {
        Row b;
        b.part = Part::Build;
        std::vector<std::string> cands;
        if (p.kind == Kind::Workspace) {
            cands.push_back("build");
            for (const auto& lib : children(repo + "/libs")) cands.push_back("libs/" + lib + "/build");
        } else {
            for (const auto& c : children(repo))
                if (c == "build" || c.starts_with("cmake-build-")) cands.push_back(c);
        }
        for (const auto& c : cands)
            if (build_dir(repo, repo + "/" + c, st.st_dev, p, prog)) {
                builds.push_back(c);
                add(b, home, repo + "/" + c, "build output of " + p.name + " -- the next build makes it again", prog);
            }
        std::size_t bins = 0;
        for (const auto& i : p.repo.ignored) {   // files only (a folder entry ends in '/')
            if (i.ends_with("/") || !is_file(repo + "/" + i) || !is_elf(repo + "/" + i)) continue;
            builds.push_back(i);
            add(b, home, repo + "/" + i, "a program built from " + p.name + " -- git ignores it; the next build makes it again", prog);
            ++bins;
        }
        b.note = plural(b.items.size() - bins, "build folder", "build folders") +
                 (bins ? ", " + plural(bins, "binary", "binaries") : std::string());
        finish(b, true);
    }

    // Source: everything in the repo folder but .git -- only when it is on the remote.
    if (clean && !p.repo.cleaned) {
        Row s;
        s.part = Part::Source;
        std::vector<std::string> top_ignored;
        for (const auto& i : p.repo.ignored) {
            const std::string n = i.ends_with("/") ? i.substr(0, i.size() - 1) : i;
            if (std::find(builds.begin(), builds.end(), n) != builds.end()) continue;
            const auto slash = n.find('/');
            if (slash == std::string::npos) { top_ignored.push_back(n); s.stays.push_back(i); continue; }
            bool under_build = false;
            for (const auto& bd : builds) under_build = under_build || n.starts_with(bd + "/");
            if (!under_build) s.leaves.push_back(i);
        }
        for (const auto& c : children(repo)) {
            if (c == ".git" || c == ".gitignore" || c == "docs" || c == "scripts" || c == "fixes") continue;
            if (std::find(builds.begin(), builds.end(), c) != builds.end()) continue;
            if (std::find(top_ignored.begin(), top_ignored.end(), c) != top_ignored.end()) continue;
            add(s, home, repo + "/" + c, p.name + "'s source -- on " + p.repo.host + " at " +
                p.repo.head.substr(0, 7) + "; git restore . brings it back", prog);
        }
        s.note = "everything in " + (p.app.empty() ? p.name : p.app) + "/ but .git and .gitignore -- git restore . brings it back";
        finish(s, false);
    }
}

void walk(const std::string& home, const std::string& rel, dev_t dev, int depth, Found& f, scan::Progress& prog,
          const std::vector<std::string>& known) {
    if (depth > 4 || prog.cancel.load()) return;
    const std::string abs = home + "/" + rel;
    prog.dirs.fetch_add(1, std::memory_order_relaxed);

    std::string app;
    if (is_workspace(abs, app)) {
        Project p;
        p.kind = Kind::Workspace;
        p.rel = rel;
        p.app = app;
        p.name = app;
        p.repo_rel = rel + "/" + app;
        const std::string a = abs + "/" + app;
        if (exists(a + "/.git")) p.repo = verify(a, &prog.cancel);
        else { p.repo.status = Status::NotRepo; p.repo.error = app + "/ has no .git"; }
        p.group = group_of(p.repo);
        if (p.repo.cleaned) {   // nothing to lint: the source is on the remote
            if (is_dir(abs + "/fixes"))
                if (const auto n = count_files(abs + "/fixes")) {
                    p.fixes_files = n;
                    p.drift.push_back({Tier::Note, "fixes/ holds " + plural(n, "file", "files") + " -- Rouster never touches it"});
                }
        } else {
            p.drift = lint(abs, app, known);
            for (const auto& d : p.drift)
                if (d.tier == Tier::Note && d.what.starts_with("fixes/")) p.fixes_files = count_files(abs + "/fixes");
        }
        rows_of(home, p, prog);
        f.projects.push_back(std::move(p));
        prog.files.fetch_add(1);
        return;
    }
    if (exists(abs + "/.git")) {
        Project p;
        p.kind = Kind::Repo;
        p.rel = rel;
        p.name = rel.substr(rel.rfind('/') + 1);
        p.repo_rel = rel;
        p.repo = verify(abs, &prog.cancel);
        p.group = group_of(p.repo);
        rows_of(home, p, prog);
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

const char* group_name(Group g) {
    switch (g) {
        case Group::Clean:    return "Safe to clean";
        case Group::NoGit:    return "No git -- review";
        case Group::LookInto: return "Look into";
    }
    return "?";
}
Group group_of(const Repo& r) {
    switch (r.status) {
        case Status::Backed:   return Group::Clean;
        case Status::NoRemote:
        case Status::NotRepo:  return Group::NoGit;
        default:               return Group::LookInto;
    }
}
const char* part_name(Part p) {
    switch (p) {
        case Part::Zips:   return "Zips";
        case Part::Build:  return "Build output";
        case Part::Source: return "Source";
    }
    return "?";
}
const Row* row_of(const Project& p, Part part) {
    for (const auto& r : p.rows) if (r.part == part) return &r;
    return nullptr;
}
Row* row_of(Project& p, Part part) {
    for (auto& r : p.rows) if (r.part == part) return &r;
    return nullptr;
}

std::string why_red(const Project& p) {
    if (p.group != Group::LookInto) return "";
    const Repo& r = p.repo;
    if (r.status == Status::Unreachable)
        return "Couldn't reach " + r.remote + " (" + r.host + "): " + r.error + " -- unknown, so not offered.";
    if (dirty(r)) {
        for (const auto& c : r.checks)
            if (c.name == "committed") return "Dirty -- " + c.detail + ". Rouster won't commit or push it; it's yours to look at.";
    }
    std::string why;
    for (const auto& c : r.checks)
        if (!c.ok) why += (why.empty() ? "" : "; ") + c.detail;
    if (why.empty()) why = r.error.empty() ? std::string(status_name(r.status)) : r.error;
    if (push_plan(p).why_not.empty()) {
        bool stash = false;
        for (const auto& c : r.checks) stash = stash || (c.name == "stashes" && !c.ok);
        why += stash ? " -- Push... sends the commits; the stash stays here" : " -- Push... is all it needs";
    }
    return why + ".";
}

door::Plan clean_plan(const std::string& home, const std::vector<Project>& projects) {
    door::Plan p;
    std::vector<std::string> froms;
    std::size_t n = 0;
    for (const auto& pr : projects) {
        if (pr.group == Group::LookInto) continue;   // red: never, ticked or not
        bool any = false;
        for (const auto& row : pr.rows) {
            if (!row.offered || !row.checked) continue;
            if (row.part == Part::Source && pr.group != Group::Clean) continue;
            for (const auto& it : row.items) {
                door::Action a;
                a.verb = door::Verb::Trash;
                a.from = home + "/" + it.rel;
                a.reason = it.why;
                a.bytes = it.bytes;
                p.actions.push_back(std::move(a));
                any = true;
            }
        }
        if (any) ++n;
    }
    // A folder going whole takes what is inside it: the inner action is left out.
    std::vector<bool> inside(p.actions.size(), false);
    for (std::size_t i = 0; i < p.actions.size(); ++i)
        for (std::size_t j = 0; j < p.actions.size(); ++j)
            if (i != j && p.actions[i].from.starts_with(p.actions[j].from + "/")) inside[i] = true;
    std::vector<door::Action> kept;
    for (std::size_t i = 0; i < p.actions.size(); ++i)
        if (!inside[i]) kept.push_back(std::move(p.actions[i]));
    p.actions = std::move(kept);
    p.label = "Clean " + plural(n, "project", "projects");
    return p;
}

std::vector<const Project*> source_ticked(const std::vector<Project>& projects) {
    std::vector<const Project*> v;
    for (const auto& pr : projects)
        if (const Row* r = row_of(pr, Part::Source); r && pr.group == Group::Clean && r->offered && r->checked && !r->items.empty())
            v.push_back(&pr);
    return v;
}

}  // namespace rouster::core::dev
