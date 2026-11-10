#include "core/Versions.hpp"
#include "core/Catalog.hpp"
#include "core/Dups.hpp"      // copy_like: the same "x (1)" / "Copy of x" Duplicates knows
#include "core/Package.hpp"
#include "core/Scan.hpp"   // samson_places

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json.hpp"

// Versions.cpp -- READ-ONLY: the catalog through catalog::query (read-only),
// lstat / opendir, and reading bytes (same_bytes, the apps' prefs). The two
// decision calls go through catalog::decide -- Rouster's own file.

namespace rouster::core::versions {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::uint64_t u(const std::string& s) { return s.empty() ? 0 : std::stoull(s); }
std::int64_t  i(const std::string& s) { return s.empty() ? 0 : std::stoll(s); }

std::string parent_of(const std::string& rel) {
    const auto k = rel.rfind('/');
    return k == std::string::npos ? "" : rel.substr(0, k);
}
std::string name_of(const std::string& rel) {
    const auto k = rel.rfind('/');
    return k == std::string::npos ? rel : rel.substr(k + 1);
}
std::string join(const std::string& folder, const std::string& name) { return folder.empty() ? name : folder + "/" + name; }

bool is_dir(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0;
}
bool starts_dir(const std::string& rel, const std::string& top) {
    return rel.compare(0, top.size(), top) == 0 && (rel.size() == top.size() || rel[top.size()] == '/');
}

std::string read_small(const std::string& path, std::size_t max = 1 << 20) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string s(max, '\0');
    in.read(s.data(), static_cast<std::streamsize>(max));
    s.resize(static_cast<std::size_t>(in.gcount()));
    return s;
}

}  // namespace

// ── names ────────────────────────────────────────────────────────────────────
void split_name(const std::string& name, std::string& stem, std::string& ext) {
    const std::string l = lower(name);
    if (l.size() > 9 && l.ends_with(".folio.gz")) {
        stem = name.substr(0, name.size() - 9);
        ext = "folio.gz";
        return;
    }
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) { stem = name; ext.clear(); return; }
    stem = name.substr(0, dot);
    ext = lower(name.substr(dot + 1));
}

std::string base_of(const std::string& stem, bool* marked, bool* copyish) {
    // Each pattern takes ONE mark off the end; we go round until none does.
    // (Written without "copy" against an open paren: the source gate reads that as a call.)
    static const std::regex copy_tail(R"((\s*\(\d{1,3}\)|[ _-]+cop[y]([ _-]*\d+)?|\s*\(copy\)| - cop[y]( \(\d+\))?)$)",
                                      std::regex::icase);
    static const std::regex numbered(
        R"([ _.-]+(v|ver|version|rev|revision|draft|final|take)[ _.-]*\d+([._]\d+)*$)", std::regex::icase);
    static const std::regex worded(
        R"([ _-]+(final|finished|draft|old|new|edit|edited|revised|latest|backup|bak|orig|original)$)", std::regex::icase);
    static const std::regex copy_head(R"(^copy of )", std::regex::icase);
    std::string s = stem;
    bool any = false, cp = false;
    for (int round = 0; round < 12; ++round) {
        std::smatch m;
        std::string next;
        if (std::regex_search(s, m, copy_head)) { next = s.substr(m.length(0)); cp = true; }
        else if (std::regex_search(s, m, copy_tail)) { next = s.substr(0, static_cast<std::size_t>(m.position(0))); cp = true; }
        else if (std::regex_search(s, m, numbered)) next = s.substr(0, static_cast<std::size_t>(m.position(0)));
        else if (std::regex_search(s, m, worded)) next = s.substr(0, static_cast<std::size_t>(m.position(0)));
        else break;
        if (next.empty()) break;   // "final" alone is a name, not a mark
        s = next;
        any = true;
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '_' || s.back() == '-' || s.back() == '.')) s.pop_back();
    if (s.empty()) { s = stem; any = false; cp = false; }
    if (marked) *marked = any;
    if (copyish) *copyish = cp;
    return s;
}

std::string versions_name(const std::string& base) { return base + " versions"; }

bool is_versions_folder(const std::string& n) {
    const std::string l = lower(n);
    return l == "versions" || (l.size() > 9 && l.ends_with(" versions"));
}

std::string Family::key() const { return folder + "/" + lower(base) + "." + ext; }

// ── app-managed history ──────────────────────────────────────────────────────
namespace {

bool folio_stamp(const std::string& s, std::size_t at) {   // YYYY-MM-DDTHH-MM at `at`
    static const char* shape = "dddd-dd-ddTdd-dd";
    for (std::size_t k = 0; shape[k]; ++k) {
        if (at + k >= s.size()) return false;
        const char c = s[at + k];
        if (shape[k] == 'd' ? !std::isdigit(static_cast<unsigned char>(c)) : c != shape[k]) return false;
    }
    return true;
}
bool is_day(const std::string& n) {   // YYYY-MM-DD
    if (n.size() != 10) return false;
    for (std::size_t k = 0; k < 10; ++k) {
        const bool dash = k == 4 || k == 7;
        if (dash ? n[k] != '-' : !std::isdigit(static_cast<unsigned char>(n[k]))) return false;
    }
    return true;
}
std::uint64_t du(const std::string& path, dev_t dev, std::set<std::pair<dev_t, ino_t>>& seen, int depth = 0) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0 || st.st_dev != dev) return 0;
    if (!S_ISDIR(st.st_mode)) {
        if (st.st_nlink > 1 && !seen.insert({st.st_dev, st.st_ino}).second) return 0;   // jot's hard-linked days: once
        return static_cast<std::uint64_t>(st.st_blocks) * 512;
    }
    std::uint64_t b = static_cast<std::uint64_t>(st.st_blocks) * 512;
    if (depth > 64) return b;
    if (DIR* d = ::opendir(path.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            b += du(path + "/" + n, dev, seen, depth + 1);
        }
        ::closedir(d);
    }
    return b;
}
std::uint64_t du(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return 0;
    std::set<std::pair<dev_t, ino_t>> seen;
    return du(path, st.st_dev, seen);
}
std::uint64_t apparent(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<std::uint64_t>(st.st_size) : 0;
}
std::int64_t mtime(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 ? st.st_mtim.tv_sec : 0;
}
std::vector<std::string> entries(const std::string& dir) {
    std::vector<std::string> v;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") v.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}
std::string resolve(const std::string& home, std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p.rfind("~/", 0) == 0) p = home + p.substr(1);
    return p;
}
bool visible_in_home(const std::string& home, const std::string& abs) {
    if (abs.size() <= home.size() + 1 || abs.compare(0, home.size() + 1, home + "/") != 0) return false;
    const std::string rel = abs.substr(home.size() + 1);
    std::size_t pos = 0;
    while (pos < rel.size()) {
        if (rel[pos] == '.') return false;
        const auto s = rel.find('/', pos);
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    return true;
}
std::string ini_value(const std::string& text, const std::string& group, const std::string& key) {
    std::istringstream in(text);
    std::string line, cur;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && line.front() == '[' && line.back() == ']') { cur = line.substr(1, line.size() - 2); continue; }
        if (cur != group) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        if (k != key) continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        return v;
    }
    return {};
}

}  // namespace

std::vector<History> app_history(const std::string& home, const std::string& data_home, const std::string& config_home) {
    std::vector<History> out;
    const auto finish = [&](History h) {
        h.in_home = visible_in_home(home, h.path);
        if (h.count) out.push_back(std::move(h));
    };

    // Folio: <dir>/<stem>/<stem>_YYYY-MM-DDTHH-MM.folio.gz, or <stem>_<ts>.folio/ (a v5 bundle's copy).
    {
        std::string dir = resolve(home, ini_value(read_small(config_home + "/folio/preferences.ini"), "AutoSave", "backup-dir"));
        if (dir.empty()) dir = data_home + "/folio/backups";
        for (const auto& stem : entries(dir)) {
            const std::string sd = dir + "/" + stem;
            if (!is_dir(sd)) continue;
            History h{"Folio", stem, sd, 0, 0, 0, 0, false};
            for (const auto& n : entries(sd)) {
                if (n.size() < stem.size() + 1 + 16 || n.compare(0, stem.size() + 1, stem + "_") != 0 ||
                    !folio_stamp(n, stem.size() + 1))
                    continue;
                const std::string tail = n.substr(stem.size() + 17);
                if (tail != ".folio.gz" && tail != ".folio") continue;
                ++h.count;
                const std::int64_t t = mtime(sd + "/" + n);
                if (!h.oldest || t < h.oldest) h.oldest = t;
                if (t > h.newest) h.newest = t;
            }
            h.bytes = du(sd);
            finish(std::move(h));
        }
    }
    // jot: <dir>/<slot>/YYYY-MM-DD/ with .source naming the jots folder.
    {
        std::string dir;
        try {
            const auto j = nlohmann::json::parse(read_small(data_home + "/jot/prefs.json"), nullptr, false);
            if (j.is_object() && j.contains("backup_dir") && j["backup_dir"].is_string())
                dir = resolve(home, j["backup_dir"].get<std::string>());
        } catch (...) {}
        if (dir.empty()) dir = data_home + "/jot/backups";
        for (const auto& slot : entries(dir)) {
            const std::string sd = dir + "/" + slot;
            if (!is_dir(sd)) continue;
            std::string src = read_small(sd + "/.source", 4096);
            while (!src.empty() && (src.back() == '\n' || src.back() == '\r')) src.pop_back();
            History h{"jot", src.empty() ? slot : name_of(src), sd, 0, 0, 0, 0, false};
            for (const auto& n : entries(sd)) {
                if (!is_day(n) || !is_dir(sd + "/" + n)) continue;
                ++h.count;
                const std::int64_t t = mtime(sd + "/" + n);
                if (!h.oldest || t < h.oldest) h.oldest = t;
                if (t > h.newest) h.newest = t;
            }
            h.bytes = du(sd);
            finish(std::move(h));
        }
    }
    // Samson: its folder (sealed/ counted) -- Protected (scan::samson_places); listed so you see why it's never proposed.
    for (const auto& rel : scan::samson_places(home, config_home)) {
        const std::string dir = home + "/" + rel;
        if (!is_dir(dir)) continue;
        History h{"Samson", name_of(rel), dir, 0, 0, 0, 0, false};
        for (const auto& n : entries(dir + "/sealed")) {
            if (!n.ends_with(".smsn")) continue;
            ++h.count;
            const std::int64_t t = mtime(dir + "/sealed/" + n);
            if (!h.oldest || t < h.oldest) h.oldest = t;
            if (t > h.newest) h.newest = t;
        }
        h.bytes = du(dir);
        h.in_home = visible_in_home(home, dir);
        out.push_back(std::move(h));   // listed even with nothing sealed yet: the folder is still Samson's
    }
    // Trails: <data>/trails/recordings/*.trail -- recordings it keeps (not versions; listed so nothing proposes them).
    {
        const std::string dir = data_home + "/trails/recordings";
        History h{"Trails", "recordings", dir, 0, 0, 0, 0, false};
        for (const auto& n : entries(dir)) {
            if (n.size() < 7 || !n.ends_with(".trail")) continue;
            ++h.count;
            const std::int64_t t = mtime(dir + "/" + n);
            if (!h.oldest || t < h.oldest) h.oldest = t;
            if (t > h.newest) h.newest = t;
        }
        if (h.count) h.bytes = du(dir);
        finish(std::move(h));
    }
    return out;
}

// ── comparing two saves ──────────────────────────────────────────────────────
bool same_bytes(const std::string& a, const std::string& b) {
    struct stat sa{}, sb{};
    if (::lstat(a.c_str(), &sa) != 0 || ::lstat(b.c_str(), &sb) != 0) return false;
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode) || sa.st_size != sb.st_size) return false;
    if (sa.st_size > 256LL * 1024 * 1024) return false;
    const auto hash = [](const std::string& p, XXH128_hash_t& out) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        XXH3_state_t* st = XXH3_createState();
        XXH3_128bits_reset(st);
        std::vector<char> buf(1 << 16);
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (in.gcount() > 0) XXH3_128bits_update(st, buf.data(), static_cast<std::size_t>(in.gcount()));
        }
        out = XXH3_128bits_digest(st);
        XXH3_freeState(st);
        return true;
    };
    XXH128_hash_t ha{}, hb{};
    return hash(a, ha) && hash(b, hb) && XXH128_isEqual(ha, hb);
}

// ── find ─────────────────────────────────────────────────────────────────────
Report find(const std::string& data_dir, const std::string& home, const std::string& data_home,
            const std::string& config_home) {
    Report r;
    r.history = app_history(home, data_home, config_home);
    const auto sum = catalog::summary(data_dir);
    if (!sum.ok) { r.error = "no catalog yet -- Look in Smart Scan first"; return r; }

    const auto res = catalog::query(data_dir,
        "SELECT path, coalesce(size, 0), coalesce(last_changed, 0), package, coalesce(dev, 0), coalesce(ino, 0) "
        "FROM files WHERE gone IS NULL AND " CATALOG_NOT_HANDS_OFF, {}, 2000000);
    if (!res.ok) { r.error = res.error; return r; }

    // Newest decision per file (keeper / not-family; anything newer wins).
    std::map<std::string, std::pair<std::string, std::string>> dec;
    {
        const auto d = catalog::query(data_dir,
            "SELECT f.path, d.verdict, coalesce(d.detail, '') FROM decisions d JOIN files f ON f.id = d.file_id "
            "WHERE f.gone IS NULL AND d.id = (SELECT max(id) FROM decisions WHERE file_id = d.file_id)", {}, 2000000);
        if (d.ok)
            for (const auto& row : d.rows) dec[row[0]] = {row[1], row[2]};
    }

    static const std::vector<std::string> skip_tops = {"Develop", "Projects", "src", "dev", "code", "Closet", "Archive"};
    std::vector<std::string> history_rels;
    for (const auto& h : r.history)
        if (h.in_home) history_rels.push_back(h.path.substr(home.size() + 1));

    std::map<std::string, bool> project_dir;   // s010: a folder under a .rouster.json keeps its own versions
    const auto in_project = [&](const std::string& folder) {
        std::string f = folder;
        std::vector<std::string> walked;
        bool hit = false;
        while (true) {
            if (auto it = project_dir.find(f); it != project_dir.end()) { hit = it->second; break; }
            walked.push_back(f);
            if (exists((f.empty() ? home : home + "/" + f) + "/.rouster.json")) { hit = true; break; }
            if (f.empty()) break;
            f = parent_of(f);
        }
        for (const auto& w : walked) project_dir[w] = hit;
        return hit;
    };

    struct Acc { std::vector<Member> m; std::string base; };
    std::map<std::string, Acc> groups;   // folder \n lower(base) \n ext
    for (const auto& row : res.rows) {
        const std::string& rel = row[0];
        bool skip = false;
        for (const auto& t : skip_tops) skip = skip || starts_dir(rel, t);
        for (const auto& h : history_rels) skip = skip || starts_dir(rel, h);
        if (skip) continue;
        std::string folder = parent_of(rel);
        bool in_v = false;
        const std::string fname = name_of(folder);
        std::string stem, ext;
        split_name(name_of(rel), stem, ext);
        bool marked = false;
        const std::string base = base_of(stem, &marked);
        if (!folder.empty() && is_versions_folder(fname)) {
            const std::string vb = lower(fname) == "versions" ? "" : fname.substr(0, fname.size() - 9);
            if (vb.empty() || lower(vb) == lower(base)) { in_v = true; folder = parent_of(folder); }
        }
        if (in_project(folder)) continue;
        Member m;
        m.rel = rel;
        m.bytes = u(row[1]);
        m.changed = i(row[2]);
        m.package = row[3] == "1";
        m.dev = u(row[4]);
        m.ino = u(row[5]);
        m.in_versions = in_v;
        if (auto it = dec.find(rel); it != dec.end() && it->second.first == "keeper") m.chosen = true;
        Acc& a = groups[folder + "\n" + lower(base) + "\n" + ext];
        a.m.push_back(std::move(m));
        if (!in_v && (a.base.empty() || !marked)) a.base = base;   // as the unmarked one writes it
        else if (a.base.empty()) a.base = base;
    }

    for (auto& [k, a] : groups) {
        if (a.m.size() < 2) continue;
        const auto n1 = k.find('\n'), n2 = k.find('\n', n1 + 1);
        Family f;
        f.folder = k.substr(0, n1);
        f.ext = k.substr(n2 + 1);
        f.base = a.base;
        f.members = std::move(a.m);
        // Newest first; saved in the same second, the plain name before a copy's or a marked one ("cat.png"
        // before "cat copy.png") -- so a tie never makes the copy the keeper.
        const auto rank = [](const Member& m) {
            std::string st, ex;
            split_name(name_of(m.rel), st, ex);
            bool marked = false, cp = false;
            base_of(st, &marked, &cp);
            return (cp || dups::copy_like(name_of(m.rel))) ? 2 : marked ? 1 : 0;
        };
        std::sort(f.members.begin(), f.members.end(), [&](const Member& x, const Member& y) {
            if (x.changed != y.changed) return x.changed > y.changed;
            const int rx = rank(x), ry = rank(y);
            return rx != ry ? rx < ry : x.rel < y.rel;
        });
        // A versions folder holding saves of this base but under another spelling ("Tapper versions" vs
        // "tapper versions"): use the one on disk.
        f.vdir = join(f.folder, versions_name(f.base));
        for (const auto& m : f.members)
            if (m.in_versions && name_of(parent_of(m.rel)) != "versions") { f.vdir = parent_of(m.rel); break; }

        // Not a family, said so of every member?
        bool all_no = true;
        for (const auto& m : f.members) {
            const auto it = dec.find(m.rel);
            all_no = all_no && it != dec.end() && it->second.first == "not-family" && it->second.second == f.key();
        }
        if (all_no) continue;

        // The keeper.
        std::vector<std::size_t> outside;
        for (std::size_t x = 0; x < f.members.size(); ++x)
            if (!f.members[x].in_versions) outside.push_back(x);
        if (outside.empty()) {
            f.keeper = 0;
            f.keeper_why = "the newest (all of them are in the versions folder)";
        } else if (outside.size() == 1) {
            f.keeper = outside[0];
            f.keeper_why = "the only one outside its versions folder";
        } else {
            f.keeper = outside[0];
            f.keeper_why = "the newest";
            for (auto x : outside)
                if (f.members[x].chosen) { f.keeper = x; f.keeper_why = "you chose"; break; }
        }
        for (std::size_t x = 0; x < f.members.size(); ++x) {
            if (x == f.keeper || f.members[x].in_versions) continue;
            ++f.loose;
            f.loose_bytes += f.members[x].bytes;
        }

        // Reflex saves among the older ones.
        for (std::size_t x = 0; x < f.members.size(); ++x) {
            if (x == f.keeper) continue;
            Member& m = f.members[x];
            std::string stem, ext;
            split_name(name_of(m.rel), stem, ext);
            if (dups::copy_like(name_of(m.rel))) { m.reflex = "a copy's name"; continue; }
            // The same bytes as an OLDER save: the later one is the reflex (the first of a pair is the original).
            if (!m.package)
                for (std::size_t y = x + 1; y < f.members.size() && m.reflex.empty(); ++y)
                    if (!f.members[y].package && same_bytes(home + "/" + m.rel, home + "/" + f.members[y].rel))
                        m.reflex = "the same bytes as " + name_of(f.members[y].rel);
            if (!m.reflex.empty() || x == 0 || m.package || f.members[x - 1].package) continue;
            const Member& newer = f.members[x - 1];   // the next one saved after it
            const std::int64_t gap = newer.changed - m.changed;
            // Apparent sizes (the catalog's are disk usage -- block-rounded, too coarse for "nearly the same").
            const std::uint64_t a1 = apparent(home + "/" + newer.rel), a2 = apparent(home + "/" + m.rel);
            const std::uint64_t big = std::max(a1, a2), small = std::min(a1, a2);
            if (gap >= 0 && gap <= 10 * 60 && big > 0 && (big - small) * 100 <= big)
                m.reflex = "saved " + (gap < 60 ? std::string("under a minute") : std::to_string(gap / 60) + " min") +
                           " before " + name_of(newer.rel) + ", nearly the same size";
        }
        r.loose += f.loose;
        r.loose_bytes += f.loose_bytes;
        r.families.push_back(std::move(f));
    }
    std::sort(r.families.begin(), r.families.end(), [](const Family& a, const Family& b) {
        if ((a.loose > 0) != (b.loose > 0)) return a.loose > 0;
        if (a.loose_bytes != b.loose_bytes) return a.loose_bytes > b.loose_bytes;
        return a.key() < b.key();
    });
    for (const auto& h : r.history)
        if (h.in_home) r.notes.push_back(h.app + "'s history is in ~/" + h.path.substr(home.size() + 1) +
                                         " -- recognised, left alone");
    r.ok = true;
    return r;
}

// ── the step ─────────────────────────────────────────────────────────────────
namespace {

// Look at a member again: "" if it is as the Look saw it.
std::string recheck(const std::string& home, const Member& m) {
    struct stat st{};
    if (::lstat((home + "/" + m.rel).c_str(), &st) != 0) return "gone since the Look";
    if (st.st_ino != m.ino || (m.dev && st.st_dev != m.dev)) return "a different file is there now";
    if (!m.package && (!S_ISREG(st.st_mode) || st.st_mtim.tv_sec != m.changed)) return "changed since the Look";
    if (m.package && !S_ISDIR(st.st_mode)) return "a different file is there now";
    return {};
}

void one(const std::string& home, const Family& f, std::size_t keeper, Step& s, std::set<std::string>& taken) {
    if (keeper >= f.members.size()) { s.dropped.push_back(f.base + ": no such keeper"); return; }
    std::vector<door::Action> acts;
    std::vector<std::string> dropped;
    std::size_t moves = 0;
    std::uint64_t bytes = 0;
    const Member& k = f.members[keeper];
    const std::string vabs = home + "/" + f.vdir;
    if (exists(vabs) && !is_dir(vabs)) { s.dropped.push_back("~/" + f.vdir + ": there, and not a folder"); return; }
    bool need_vdir = !exists(vabs) && !taken.count(vabs);

    if (k.in_versions) {   // the keeper comes out, beside where the versions folder is
        const std::string why = recheck(home, k);
        const std::string to = join(f.folder, name_of(k.rel));
        if (!why.empty()) { s.dropped.push_back(k.rel + ": " + why + " -- nothing planned for " + f.base); return; }
        if (exists(home + "/" + to) || taken.count(home + "/" + to)) {
            s.dropped.push_back(k.rel + ": its name is taken in ~/" + (f.folder.empty() ? "" : f.folder) +
                                " -- nothing planned for " + f.base);
            return;
        }
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = home + "/" + k.rel;
        a.to = home + "/" + to;
        a.bytes = k.bytes;
        a.reason = "the keeper -- out of the versions folder";
        acts.push_back(a);
        taken.insert(a.to);
    }
    for (std::size_t x = 0; x < f.members.size(); ++x) {
        const Member& m = f.members[x];
        if (x == keeper || m.in_versions) continue;
        if (const std::string why = recheck(home, m); !why.empty()) { dropped.push_back(m.rel + ": " + why); continue; }
        const std::string to = vabs + "/" + name_of(m.rel);
        if (exists(to) || taken.count(to)) { dropped.push_back(m.rel + ": its name is taken in ~/" + f.vdir); continue; }
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = home + "/" + m.rel;
        a.to = to;
        a.bytes = m.bytes;
        a.reason = m.reflex.empty() ? "an older save of " + name_of(k.rel) : "an older save -- " + m.reflex;
        acts.push_back(a);
        taken.insert(to);
        ++moves;
        bytes += m.bytes;
    }
    // When the keeper sat inside the versions folder, the old outside one moves in too (above) -- the swap.
    if (!moves && !k.in_versions) { for (auto& d : dropped) s.dropped.push_back(d); return; }
    if (need_vdir && moves) {
        door::Action d;
        d.verb = door::Verb::MakeDir;
        d.to = vabs;
        d.reason = "the versions folder for " + name_of(k.rel);
        s.plan.actions.push_back(d);
        taken.insert(vabs);
    }
    for (auto& a : acts) s.plan.actions.push_back(std::move(a));
    for (auto& d : dropped) s.dropped.push_back(d);
    s.moves += moves;
    s.bytes += bytes;
}

}  // namespace

Step plan(const std::string& home, const Family& f, std::size_t keeper) {
    Step s;
    std::set<std::string> taken;
    one(home, f, keeper, s, taken);
    s.plan.label = "Versions: " + f.base + (f.ext.empty() ? "" : "." + f.ext) + " -- older saves beside it";
    return s;
}

Step plan_all(const std::string& home, const std::vector<Family>& fs, const std::vector<std::size_t>& keepers) {
    Step s;
    std::set<std::string> taken;
    for (std::size_t x = 0; x < fs.size(); ++x) one(home, fs[x], x < keepers.size() ? keepers[x] : fs[x].keeper, s, taken);
    s.plan.label = fs.size() == 1 ? "Versions: " + fs[0].base + (fs[0].ext.empty() ? "" : "." + fs[0].ext) + " -- older saves beside it"
                                  : "Versions: " + std::to_string(fs.size()) + " families -- older saves beside their keepers";
    return s;
}

bool choose_keeper(const std::string& data_dir, const Family& f, std::size_t keeper, std::int64_t now) {
    if (keeper >= f.members.size()) return false;
    return catalog::decide(data_dir, f.members[keeper].rel, "keeper", f.key(), now);
}

bool not_a_family(const std::string& data_dir, const Family& f, std::int64_t now) {
    bool ok = !f.members.empty();
    for (const auto& m : f.members) ok = catalog::decide(data_dir, m.rel, "not-family", f.key(), now) && ok;
    return ok;
}

}  // namespace rouster::core::versions
