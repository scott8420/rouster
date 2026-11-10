#include "core/Tidy.hpp"
#include "core/Archive.hpp"   // destination(), the Archive's place
#include "core/Catalog.hpp"
#include "core/Rules.hpp"     // in_progress(): a partial download isn't in the Closet yet
#include "core/Scan.hpp"   // human_bytes

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Tidy.cpp -- the report. Read-only: SQL through catalog::query (opened
// read-only, writing statements refused), and opendir / lstat on the top of
// home. Nothing here writes.

namespace rouster::core::tidy {

namespace {

// Shared pieces of every verdict query: your files there now, not decided
// "keep-here", with their last use.
#define TIDY_LU "max(coalesce(last_read, 0), coalesce(last_changed, 0))"
#define TIDY_BASE                                                                                  \
    "SELECT path, coalesce(size, 0), " TIDY_LU ", package, coalesce(dev, 0), coalesce(ino, 0), "              \
    "coalesce(last_changed, 0) FROM files "                           \
    "WHERE gone IS NULL AND id NOT IN (SELECT file_id FROM decisions d WHERE file_id IS NOT NULL " \
    "AND verdict = 'keep-here' AND d.id = (SELECT max(id) FROM decisions WHERE file_id = d.file_id)) AND "   \
    "path NOT GLOB 'Develop/*' AND path NOT GLOB 'Projects/*' AND path NOT GLOB 'src/*' AND "          \
    "path NOT GLOB 'dev/*' AND path NOT GLOB 'code/*' AND path NOT GLOB 'Closet/*' AND "               \
    CATALOG_NOT_HANDS_OFF " AND "

}  // namespace

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Short:     return "Short life";
        case Verdict::Dormant:   return "Dormant";
        case Verdict::Undecided: return "Undecided";
    }
    return "?";
}

const char* verdict_why(Verdict v) {
    switch (v) {
        case Verdict::Short:     return "Useful once, done now -- to the Closet, where each waits 30 days and is then offered to the Trash at a review.";
        case Verdict::Dormant:   return "Neither opened nor changed in 18 months -- kept, finished: to ~/Archive/<year>/, their folders kept.";
        case Verdict::Undecided: return "Loose at the top of home or on the Desktop -- no home yet: gathered into the Closet to be sorted.";
    }
    return "";
}

const std::vector<Query>& queries() {
    static const std::vector<Query> q = {
        {"installers", "Installers and disk images", Verdict::Short,
         TIDY_BASE "package = 0 AND ext IN ('iso','img','dmg','exe','msi','deb','rpm','appimage','pkg','run',"
                   "'flatpakref') AND " TIDY_LU " < :now - :short"},
        {"screenshots", "Old screenshots", Verdict::Short,
         TIDY_BASE "package = 0 AND ext IN ('png','jpg','jpeg','webp') AND (path GLOB 'Screenshot*' OR "
                   "path GLOB '*/Screenshot*' OR path GLOB 'Screen Shot*' OR path GLOB '*/Screen Shot*') AND "
                   TIDY_LU " < :now - :short"},
        {"reflex", "Reflex saves (\"x (1)\", \"Copy of x\")", Verdict::Short,
         TIDY_BASE "package = 0 AND (path GLOB '* ([0-9]).*' OR path GLOB '* ([0-9][0-9]).*' OR "
                   "path GLOB '*/Copy of *' OR path GLOB 'Copy of *' OR path GLOB '* copy.*' OR "
                   "path GLOB '* - Copy.*')"},
        {"dormant", "Unused 18 months", Verdict::Dormant,
         TIDY_BASE TIDY_LU " < :now - :dormant AND path NOT GLOB 'Archive/*'"},
        {"loose", "Loose at the top of home or on the Desktop", Verdict::Undecided,
         TIDY_BASE "(instr(path, '/') = 0 OR (path GLOB 'Desktop/*' AND instr(substr(path, 9), '/') = 0))"},
    };
    return q;
}

std::string age(std::int64_t s) {
    if (s < 0) s = 0;
    const std::int64_t days = s / 86400;
    char b[48];
    if (days < 60)       std::snprintf(b, sizeof b, "%lld day%s", static_cast<long long>(days), days == 1 ? "" : "s");
    else if (days < 730) std::snprintf(b, sizeof b, "%lld months", static_cast<long long>(days / 30));
    else                 std::snprintf(b, sizeof b, "%lld years", static_cast<long long>(days / 365));
    return b;
}

namespace {

std::uint64_t u(const std::string& s) { return s.empty() ? 0 : std::stoull(s); }
std::int64_t  i(const std::string& s) { return s.empty() ? 0 : std::stoll(s); }

std::string parent_of(const std::string& rel) {
    const auto s = rel.rfind('/');
    return s == std::string::npos ? std::string{} : rel.substr(0, s);
}
std::string first_of(const std::string& rel) {
    const auto s = rel.find('/');
    return s == std::string::npos ? std::string{} : rel.substr(0, s);
}
bool is_dir(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ~/.config/user-dirs.dirs: XDG_DOWNLOAD_DIR="$HOME/Downloads" -> {"DOWNLOAD", "Downloads"}.
std::vector<std::pair<std::string, std::string>> user_dirs(const std::string& home) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(home + "/.config/user-dirs.dirs");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XDG_", 0) != 0) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.size() < eq + 3) continue;
        std::string key = line.substr(4, eq - 4);
        if (key.size() > 4 && key.ends_with("_DIR")) key.resize(key.size() - 4);
        std::string val = line.substr(eq + 1);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (val.rfind("$HOME", 0) == 0) val = val.substr(5);
        else if (val.rfind(home, 0) == 0) val = val.substr(home.size());
        else { out.emplace_back(key, val); continue; }   // outside home: kept as written
        if (!val.empty() && val.front() == '/') val = val.substr(1);
        out.emplace_back(key, val);
    }
    return out;
}

}  // namespace

Report report(const std::string& data_dir, const std::string& home, std::int64_t now) {
    Report r;
    const auto sum = catalog::summary(data_dir);
    if (!sum.ok) { r.error = "no catalog yet -- Look first"; return r; }
    const std::vector<catalog::Param> params = {{":now", now}, {":short", kShortAge}, {":dormant", kDormantAge}};

    // Everything of yours there now: totals, folder health, the top of home.
    const auto all = catalog::query(data_dir,
        "SELECT path, coalesce(size, 0), coalesce(kind, ''), coalesce(ext, ''), coalesce(born, first_seen), "
        TIDY_LU ", package FROM files WHERE gone IS NULL", {}, 2000000);
    if (!all.ok) { r.error = all.error; return r; }

    for (Verdict v : {Verdict::Short, Verdict::Dormant, Verdict::Undecided}) r.groups.push_back({v, 0, 0, {}});
    std::unordered_set<std::string> claimed;
    for (const auto& q : queries()) {
        const auto res = catalog::query(data_dir, q.sql, params, 2000000);
        if (!res.ok) { r.notes.push_back("query '" + q.id + "' failed: " + res.error); continue; }
        Group& g = r.groups[static_cast<std::size_t>(q.verdict)];
        for (const auto& row : res.rows) {
            if (!claimed.insert(row[0]).second) continue;   // an earlier query has it
            const std::uint64_t b = u(row[1]);
            g.rows.push_back({row[0], b, i(row[2]), q.id, row[3] == "1", u(row[4]), u(row[5]), i(row[6])});
            ++g.files;
            g.bytes += b;
        }
    }
    for (auto& g : r.groups)
        std::sort(g.rows.begin(), g.rows.end(), [](const Row& a, const Row& b) {
            return a.bytes != b.bytes ? a.bytes > b.bytes : a.rel < b.rel;
        });
    {
        const auto kept = catalog::query(data_dir,
            "SELECT count(DISTINCT d.file_id) FROM decisions d JOIN files f ON f.id = d.file_id WHERE f.gone IS NULL AND "
            "d.verdict = 'keep-here' AND d.id = (SELECT max(id) FROM decisions WHERE file_id = d.file_id)");
        if (kept.ok && !kept.rows.empty()) r.kept = u(kept.rows[0][0]);
    }

    r.hands_off = catalog::hands_off(data_dir);
    struct Acc { std::uint64_t files = 0, bytes = 0, arrived = 0, unread = 0; std::set<std::string> kinds; };
    std::map<std::string, Acc> folders, tops;
    for (const auto& row : all.rows) {
        const std::uint64_t b = u(row[1]);
        ++r.files;
        r.bytes += b;
        if (catalog::under(r.hands_off, row[0])) { ++r.left_alone; r.left_alone_bytes += b; }
        else if (!claimed.count(row[0])) { ++r.active; r.active_bytes += b; }
        const std::string& kind = row[2];
        const std::string major = kind.empty() ? "ext:" + row[3] : kind.substr(0, kind.find('/'));
        Acc& f = folders[parent_of(row[0])];
        ++f.files;
        f.bytes += b;
        if (i(row[4]) > now - 30 * 86400) ++f.arrived;
        if (i(row[5]) < now - 365 * 86400) ++f.unread;
        f.kinds.insert(major);
        const std::string top = first_of(row[0]);
        if (top.empty()) r.loose.push_back(row[0]);
        else { Acc& t = tops[top]; ++t.files; t.bytes += b; }
    }
    std::sort(r.loose.begin(), r.loose.end());

    for (const auto& [rel, a] : folders) {
        if (rel == kCloset || rel.rfind(std::string(kCloset) + "/", 0) == 0) continue;   // s016b: the Closet has its own card
        Folder f{rel, a.files, a.bytes, a.arrived, a.kinds.size(), a.unread, 0, ""};
        const double unread_share = a.files ? static_cast<double>(a.unread) / static_cast<double>(a.files) : 0;
        f.score = static_cast<double>(a.arrived) +
                  3.0 * static_cast<double>(a.kinds.size() > 2 ? a.kinds.size() - 2 : 0) +
                  (a.files >= 5 ? 10.0 * unread_share : 0.0) + (rel.empty() ? 0.5 * static_cast<double>(a.files) : 0.0);
        std::vector<std::string> bits;
        if (rel.empty())        bits.push_back(std::to_string(a.files) + " loose at the top of home");
        if (a.arrived)          bits.push_back(std::to_string(a.arrived) + " arrived this month");
        if (a.kinds.size() > 2) bits.push_back(std::to_string(a.kinds.size()) + " kinds mixed");
        if (a.files >= 5 && a.unread)
            bits.push_back(std::to_string(static_cast<int>(unread_share * 100 + 0.5)) + "% unused in a year");
        for (std::size_t k = 0; k < bits.size(); ++k) f.why += (k ? " · " : "") + bits[k];
        if (f.score >= 3.0 && a.files >= 3 && !catalog::under(r.hands_off, rel)) r.folders.push_back(std::move(f));
    }
    std::sort(r.folders.begin(), r.folders.end(), [](const Folder& a, const Folder& b) {
        return a.score != b.score ? a.score > b.score : a.rel < b.rel;
    });
    if (r.folders.size() > 12) r.folders.resize(12);

    // The top of home, from the disk (a folder with nothing of yours cataloged still shows).
    if (DIR* d = ::opendir(home.c_str())) {
        static const std::set<std::string> dev_roots = {"Develop", "Projects", "src", "dev", "code"};
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || !is_dir(home + "/" + n)) continue;
            Top t{n, 0, 0, ""};
            if (auto it = tops.find(n); it != tops.end()) { t.files = it->second.files; t.bytes = it->second.bytes; }
            struct stat g{};
            if (catalog::under(r.hands_off, n)) t.note = "hands off";
            else if (n == "Archive") t.note = "the Archive";
            else if (n == "Closet") t.note = "the Closet";
            else if (t.files == 0 && (dev_roots.count(n) || ::lstat((home + "/" + n + "/.git").c_str(), &g) == 0))
                t.note = "code -- Dev projects' (not cataloged)";
            else if (t.files == 0) t.note = "nothing of yours cataloged";
            r.top.push_back(std::move(t));
        }
        ::closedir(d);
    }
    std::sort(r.top.begin(), r.top.end(), [](const Top& a, const Top& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name;
    });

    // Drift.
    for (const auto& [key, rel] : user_dirs(home)) {
        if (!rel.empty() && rel.front() == '/') continue;
        if (rel.empty()) { if (key != "DESKTOP") r.drift.push_back(key + " folder is home itself -- files of that kind land loose at the top"); continue; }
        if (!is_dir(home + "/" + rel)) r.drift.push_back(key + " folder points at ~/" + rel + ", which isn't there");
        if (key == "DOWNLOAD" && rel != "Closet") r.drift.push_back("Downloads land in ~/" + rel + ", not the Closet");
    }
    if (!is_dir(home + "/Closet")) r.drift.push_back("No ~/Closet yet -- the landing place for new files");

    r.ok = true;
    return r;
}


// ── s016: the acts ───────────────────────────────────────────────────────────
namespace {

std::string base_of(const std::string& rel) {
    const auto s = rel.rfind('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}

// "Desktop/notes.txt" -> "notes (from Desktop).txt"; a top-level file -> "(from home)".
std::string from_name(const std::string& rel) {
    const std::string name = base_of(rel), par = parent_of(rel);
    const std::string where = par.empty() ? "home" : base_of(par);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name + " (from " + where + ")";
    return name.substr(0, dot) + " (from " + where + ")" + name.substr(dot);
}

// MakeDir for every missing folder from home/<first> down to `dir_rel`, parents first, once.
void need_dirs(const std::string& home, const std::string& dir_rel, std::set<std::string>& planned,
               std::vector<door::Action>& out, const std::string& why) {
    std::size_t pos = 0;
    while (true) {
        const auto s = dir_rel.find('/', pos);
        const std::string abs = home + "/" + dir_rel.substr(0, s);
        struct stat st{};
        if (::lstat(abs.c_str(), &st) != 0 && planned.insert(abs).second) {
            door::Action a;
            a.verb = door::Verb::MakeDir;
            a.to = abs;
            a.reason = why;
            out.push_back(a);
        }
        if (s == std::string::npos) break;
        pos = s + 1;
    }
}

bool exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
}

}  // namespace

Act plan(const std::string& home, Verdict v, const std::vector<Row>& rows) {
    Act act;
    act.plan.label = v == Verdict::Short     ? "Tidy: short life to the Closet"
                   : v == Verdict::Undecided ? "Tidy: gather into the Closet"
                                             : "Tidy: dormant to the Archive";
    std::set<std::string> planned;       // folders this plan makes
    std::set<std::string> taken;         // destinations this plan fills
    std::vector<door::Action> dirs, moves;
    if (v != Verdict::Dormant) {
        need_dirs(home, kCloset, planned, dirs, "the Closet -- the landing place");
        need_dirs(home, kJunkDrawer, planned, dirs, "the Junk Drawer -- kept things with no home yet");
    }
    for (const auto& row : rows) {
        const std::string from = home + "/" + row.rel;
        struct stat st{};
        if (::lstat(from.c_str(), &st) != 0) { act.dropped.push_back(row.rel + ": gone since the Look"); continue; }
        if (st.st_ino != row.ino || (row.dev && st.st_dev != row.dev)) {
            act.dropped.push_back(row.rel + ": a different file is there now");
            continue;
        }
        if (!row.package && (!S_ISREG(st.st_mode) || st.st_mtim.tv_sec != row.changed)) {
            act.dropped.push_back(row.rel + ": changed since the Look");
            continue;
        }
        std::string to_rel;
        if (v == Verdict::Dormant) {
            std::tm tmv{};
            const std::time_t t = st.st_mtim.tv_sec;
            ::localtime_r(&t, &tmv);
            to_rel = archive::destination(row.rel, tmv.tm_year + 1900);
            if (exists(home + "/" + to_rel) || taken.count(to_rel)) {
                act.dropped.push_back(row.rel + ": its place in the Archive is taken");
                continue;
            }
            need_dirs(home, parent_of(to_rel), planned, dirs, "a folder in the archive");
        } else {
            to_rel = std::string(kCloset) + "/" + base_of(row.rel);
            if (exists(home + "/" + to_rel) || taken.count(to_rel)) to_rel = std::string(kCloset) + "/" + from_name(row.rel);
            if (exists(home + "/" + to_rel) || taken.count(to_rel)) {
                act.dropped.push_back(row.rel + ": its name is taken in the Closet");
                continue;
            }
        }
        taken.insert(to_rel);
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = from;
        a.to = home + "/" + to_rel;
        a.bytes = row.bytes;
        a.reason = std::string(verdict_name(v)) + " -- proposed by '" + row.query + "'";
        moves.push_back(a);
        ++act.moves;
        act.bytes += row.bytes;
        if (v == Verdict::Short) act.shorts.push_back(row.rel);
    }
    if (act.moves) {
        act.plan.actions = std::move(dirs);
        act.plan.actions.insert(act.plan.actions.end(), moves.begin(), moves.end());
    }
    return act;
}

std::size_t remember(const std::string& data_dir, const std::string& home, const Act& act, std::int64_t now) {
    std::size_t n = 0;
    for (const auto& rel : act.shorts) {
        const std::string from = home + "/" + rel;
        for (const auto& a : act.plan.actions) {
            if (a.verb != door::Verb::Move || a.from != from) continue;
            if (!exists(from) && exists(a.to) &&
                catalog::decide(data_dir, rel, "short", "expires " + std::to_string(now + kShortAge), now))
                ++n;
            break;
        }
    }
    return n;
}

std::int64_t expiry_of(const std::string& detail) {
    static const std::string pre = "expires ";
    if (detail.rfind(pre, 0) != 0 || detail.size() == pre.size()) return 0;
    for (std::size_t k = pre.size(); k < detail.size(); ++k)
        if (detail[k] < '0' || detail[k] > '9') return 0;
    return std::stoll(detail.substr(pre.size()));
}

ClosetState closet_state(const std::string& home) {
    ClosetState c;
    c.closet = is_dir(home + "/" + kCloset);
    c.junk = is_dir(home + "/" + kJunkDrawer);
    c.dirs_file = exists(home + "/.config/user-dirs.dirs");
    for (const auto& [key, rel] : user_dirs(home))
        if (key == "DOWNLOAD") c.download = rel;
    c.redirected = c.download == kCloset;
    return c;
}

std::string redirect_downloads(const std::string& text) {
    static const std::string line = "XDG_DOWNLOAD_DIR=\"$HOME/Closet\"";
    std::istringstream in(text);
    std::string out, l;
    bool done = false;
    while (std::getline(in, l)) {
        if (!done && l.rfind("XDG_DOWNLOAD_DIR=", 0) == 0) { out += line + "\n"; done = true; continue; }
        out += l + "\n";
    }
    if (!done) out += line + "\n";
    return out;
}

door::Plan closet_plan(const std::string& home, bool redirect) {
    door::Plan p;
    const ClosetState c = closet_state(home);
    p.label = !c.closet ? (redirect && !c.redirected ? "Tidy: make the Closet, Downloads land there" : "Tidy: make the Closet")
                        : "Tidy: Downloads land in the Closet";
    std::set<std::string> planned;
    need_dirs(home, kCloset, planned, p.actions, "the Closet -- the landing place");
    need_dirs(home, kJunkDrawer, planned, p.actions, "the Junk Drawer -- kept things with no home yet");
    if (redirect && !c.redirected) {
        const std::string f = home + "/.config/user-dirs.dirs";
        std::string text;
        std::uint32_t mode = 0644;
        if (c.dirs_file) {
            std::ifstream in(f, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            text = ss.str();
            struct stat st{};
            if (::lstat(f.c_str(), &st) == 0) mode = st.st_mode & 0777;
            door::Action t;
            t.verb = door::Verb::Trash;
            t.from = f;
            t.bytes = text.size();
            t.reason = "the old setting (Downloads in ~/" + (c.download.empty() ? std::string("Downloads") : c.download) +
                       ") -- Undo brings it back";
            p.actions.push_back(t);
        } else {
            text = "# This file is written by xdg-user-dirs-update\n";
        }
        door::Action w;
        w.verb = door::Verb::Write;
        w.to = f;
        w.content = redirect_downloads(text);
        w.mode = mode;
        w.bytes = w.content.size();
        w.reason = "Downloads land in ~/Closet -- for apps started after this";
        p.actions.push_back(w);
    }
    return p;
}

// ── s016b: the Closet's review ───────────────────────────────────────────────
namespace {

// Disk usage of a folder, one filesystem, no symlinks followed (like Scan).
std::uint64_t du(const std::string& dir, dev_t dev, int depth = 0) {
    if (depth > 64) return 0;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;
    std::uint64_t n = 0;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string p = dir + "/" + name;
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0 || st.st_dev != dev) continue;
        n += static_cast<std::uint64_t>(st.st_blocks) * 512;
        if (S_ISDIR(st.st_mode)) n += du(p, dev, depth + 1);
    }
    ::closedir(d);
    return n;
}

// Look at an item again: still the same object, a file not changed.
std::string recheck(const std::string& home, const ClosetItem& it) {
    struct stat st{};
    if (::lstat((home + "/" + it.rel).c_str(), &st) != 0) return "gone since the Look";
    if (st.st_ino != it.ino || (it.dev && st.st_dev != it.dev)) return "a different file is there now";
    if (!it.dir && (!S_ISREG(st.st_mode) || st.st_mtim.tv_sec != it.changed)) return "changed since the Look";
    return {};
}

}  // namespace

bool expired(const ClosetItem& it, std::int64_t now) { return it.expires > 0 && it.expires <= now; }

ClosetView closet(const std::string& data_dir, const std::string& home, std::int64_t now) {
    ClosetView v;
    const std::string root = home + "/" + kCloset;
    v.closet = is_dir(root);
    if (!v.closet) return v;
    // The newest decision and first-seen of every cataloged file in the Closet (read-only).
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> known;   // rel -> (expires, first seen)
    const auto q = catalog::query(data_dir,
        "SELECT f.path, coalesce(f.first_seen, 0), coalesce((SELECT d.verdict || ' ' || coalesce(d.detail, '') "
        "FROM decisions d WHERE d.file_id = f.id ORDER BY d.id DESC LIMIT 1), '') "
        "FROM files f WHERE f.gone IS NULL AND f.path GLOB 'Closet/*' AND f.path NOT GLOB 'Closet/*/*'");
    if (q.ok)
        for (const auto& row : q.rows) {
            const std::string& dec = row[2];
            const std::int64_t ex = dec.rfind("short ", 0) == 0 ? expiry_of(dec.substr(6)) : 0;
            known[row[0]] = {ex, i(row[1])};
        }
    if (DIR* d = ::opendir(root.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.' || rules::in_progress(name)) continue;
            const std::string rel = std::string(kCloset) + "/" + name;
            struct stat st{};
            if (::lstat((home + "/" + rel).c_str(), &st) != 0) continue;
            if (rel == kJunkDrawer) {
                if (S_ISDIR(st.st_mode))
                    if (DIR* j = ::opendir((home + "/" + rel).c_str())) {
                        while (const dirent* je = ::readdir(j))
                            if (je->d_name[0] != '.') ++v.junk;
                        ::closedir(j);
                    }
                continue;
            }
            if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) continue;   // a symlink, a socket: not ours to judge
            ClosetItem it;
            it.rel = rel;
            it.dir = S_ISDIR(st.st_mode);
            it.dev = st.st_dev;
            it.ino = st.st_ino;
            it.changed = st.st_mtim.tv_sec;
            it.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512 + (it.dir ? du(home + "/" + rel, st.st_dev) : 0);
            if (const auto k = known.find(rel); k != known.end()) { it.expires = k->second.first; it.arrived = k->second.second; }
            if (expired(it, now)) ++v.expired; else if (it.expires) ++v.expiring;
            v.bytes += it.bytes;
            v.items.push_back(std::move(it));
        }
        ::closedir(d);
    }
    std::sort(v.items.begin(), v.items.end(), [](const ClosetItem& a, const ClosetItem& b) {
        const bool ea = a.expires > 0, eb = b.expires > 0;
        if (ea != eb) return ea;                                   // with an expiry first
        if (ea && a.expires != b.expires) return a.expires < b.expires;   // soonest (most past) first
        return a.rel < b.rel;
    });
    return v;
}

Review review_plan(const std::string& home, const std::vector<ClosetItem>& items, std::int64_t now) {
    Review r;
    r.plan.label = "Tidy: Closet review -- expired to the Trash";
    for (const auto& it : items) {
        if (it.rel.rfind(std::string(kCloset) + "/", 0) != 0 || it.rel == kJunkDrawer) {
            r.dropped.push_back(it.rel + ": not in the Closet");
            continue;
        }
        if (!expired(it, now)) { r.dropped.push_back(it.rel + ": not expired yet"); continue; }
        if (const auto why = recheck(home, it); !why.empty()) { r.dropped.push_back(it.rel + ": " + why); continue; }
        door::Action a;
        a.verb = door::Verb::Trash;
        a.from = home + "/" + it.rel;
        a.bytes = it.bytes;
        a.reason = "short life, expired " + age(now - it.expires) + " ago";
        r.plan.actions.push_back(a);
        ++r.n;
        r.bytes += it.bytes;
    }
    return r;
}

Review junk_plan(const std::string& home, const std::vector<ClosetItem>& items) {
    Review r;
    r.plan.label = "Tidy: to the Junk Drawer";
    std::set<std::string> planned, taken;
    std::vector<door::Action> dirs, moves;
    need_dirs(home, kJunkDrawer, planned, dirs, "the Junk Drawer -- kept things with no home yet");
    for (const auto& it : items) {
        if (it.rel.rfind(std::string(kCloset) + "/", 0) != 0 || it.rel == kJunkDrawer ||
            it.rel.find('/', std::string(kCloset).size() + 1) != std::string::npos) {
            r.dropped.push_back(it.rel + ": not at the top of the Closet");
            continue;
        }
        if (const auto why = recheck(home, it); !why.empty()) { r.dropped.push_back(it.rel + ": " + why); continue; }
        const std::string to_rel = std::string(kJunkDrawer) + "/" + base_of(it.rel);
        if (exists(home + "/" + to_rel) || taken.count(to_rel)) {
            r.dropped.push_back(it.rel + ": its name is taken in the Junk Drawer");
            continue;
        }
        taken.insert(to_rel);
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = home + "/" + it.rel;
        a.to = home + "/" + to_rel;
        a.bytes = it.bytes;
        a.reason = "kept, no home yet -- never ages out";
        moves.push_back(a);
        ++r.n;
        r.bytes += it.bytes;
    }
    if (r.n) {
        r.plan.actions = std::move(dirs);
        r.plan.actions.insert(r.plan.actions.end(), moves.begin(), moves.end());
    }
    return r;
}

std::vector<std::string> kept(const std::string& data_dir) {
    std::vector<std::string> out;
    const auto q = catalog::query(data_dir,
        "SELECT f.path FROM files f WHERE f.gone IS NULL AND (SELECT d.verdict FROM decisions d WHERE d.file_id = f.id "
        "ORDER BY d.id DESC LIMIT 1) = 'keep-here' ORDER BY f.path");
    if (q.ok) for (const auto& row : q.rows) out.push_back(row[0]);
    return out;
}

// ── s016c: Put away ──────────────────────────────────────────────────────────
Review put_away_plan(const std::string& home, const ClosetItem& item, const std::string& dest_rel,
                     const std::vector<std::string>& hands_off) {
    Review r;
    const std::string closet = kCloset;
    r.plan.label = "Tidy: put away " + base_of(item.rel) + " -> ~/" + dest_rel;
    const auto no = [&](const std::string& why) { r.dropped.push_back(item.rel + ": " + why); return r; };
    if (item.rel.rfind(closet + "/", 0) != 0 || item.rel == kJunkDrawer ||
        item.rel.find('/', closet.size() + 1) != std::string::npos)
        return no("not at the top of the Closet");
    if (dest_rel.empty() || dest_rel.front() == '/' || dest_rel.back() == '/' || dest_rel.find("..") != std::string::npos ||
        dest_rel.find("//") != std::string::npos)
        return no("~/" + dest_rel + " isn't a folder under home");
    if (dest_rel == closet || dest_rel.rfind(closet + "/", 0) == 0)
        return no("that's the Closet -- the Junk Drawer has its own button");
    if (catalog::under(hands_off, dest_rel)) return no("~/" + dest_rel + " is hands off");
    if (!is_dir(home + "/" + dest_rel)) return no("~/" + dest_rel + " isn't a folder there");
    if (const auto why = recheck(home, item); !why.empty()) return no(why);
    const std::string to = home + "/" + dest_rel + "/" + base_of(item.rel);
    if (exists(to)) return no("~/" + dest_rel + " already has a " + base_of(item.rel));
    door::Action a;
    a.verb = door::Verb::Move;
    a.from = home + "/" + item.rel;
    a.to = to;
    a.bytes = item.bytes;
    a.reason = "put away -- its home, chosen by you";
    r.plan.actions.push_back(a);
    r.n = 1;
    r.bytes = item.bytes;
    return r;
}

std::optional<rules::Rule> home_rule(const ClosetItem& item, const std::string& dest_rel,
                                     const std::vector<rules::Rule>& existing, std::string* why) {
    const auto no = [&](const std::string& w) -> std::optional<rules::Rule> { if (why) *why = w; return std::nullopt; };
    if (item.dir) return no("a folder -- rules sort files");
    const std::string name = base_of(item.rel);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return no("no extension to sort by");
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& e : existing)
        if (e.folder == kCloset && std::find(e.exts.begin(), e.exts.end(), ext) != e.exts.end())
            return no("the rule “" + e.name + "” already sorts *." + ext + " in the Closet");
    rules::Rule r;
    r.name = "Closet: *." + ext + " -> " + dest_rel;
    r.folder = kCloset;
    r.exts = {ext};
    r.act = rules::Do::Move;
    r.dest = dest_rel;
    r.enabled = false;
    return r;
}

}  // namespace rouster::core::tidy
