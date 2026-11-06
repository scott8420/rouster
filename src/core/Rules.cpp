#include "core/Rules.hpp"
#include "core/Archive.hpp"    // destination(): the Archive page's layout
#include "core/Organize.hpp"   // File into a project: its pattern and routes

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <sys/stat.h>

// Rules.cpp -- READ-ONLY. Matches and plans; nothing here writes (the source
// gate holds it to that). rules.json is saved by core/Settings.

namespace rouster::core::rules {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* do_name(Do d) {
    switch (d) {
        case Do::Move: return "move";
        case Do::Trash: return "trash";
        case Do::Archive: return "archive";
        case Do::File: return "file";
    }
    return "?";
}

namespace {

Do do_of(const std::string& s) {
    for (Do d : {Do::Move, Do::Trash, Do::Archive, Do::File}) if (s == do_name(d)) return d;
    return Do::Move;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string ext_of(const std::string& name) {
    const auto d = name.rfind('.');
    return d == std::string::npos || d == 0 || d + 1 == name.size() ? std::string() : lower(name.substr(d + 1));
}
std::string stem_of(const std::string& name) {
    const auto d = name.rfind('.');
    return d == std::string::npos || d == 0 || d + 1 == name.size() ? name : name.substr(0, d);
}
bool exists(const std::string& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0; }
bool is_dir(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
bool clean_rel(const std::string& r) {
    return !r.empty() && r[0] != '/' && fs::path(r).lexically_normal().string() == r && r.find("..") == std::string::npos;
}
int year_of(std::int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    return tm.tm_year + 1900;
}
void replace_all(std::string& s, const std::string& a, const std::string& b) {
    for (std::size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
}
std::string renamed(const Rule& r, const std::string& name, std::int64_t mtime) {
    if (r.rename.empty()) return name;
    std::string s = r.rename;
    const std::string stem = stem_of(name);
    const std::string ext = stem.size() < name.size() ? name.substr(stem.size() + 1) : std::string();   // as it was spelled
    replace_all(s, "{stem}", stem);
    replace_all(s, "{ext}", ext);
    s = organize::fill(s, organize::Values{"", "", mtime});
    if (ext.empty() && s.size() && s.back() == '.') s.pop_back();
    for (auto& c : s) if (c == '/') c = '-';
    return s;
}
std::string with_n(const std::string& name, int n) {
    if (n == 1) return name;
    const std::string s = stem_of(name), e = name.substr(s.size());
    return s + "-" + std::to_string(n) + e;
}
door::Action make_dir(const std::string& p) {
    door::Action a;
    a.verb = door::Verb::MakeDir;
    a.to = p;
    a.reason = "a folder a rule moves into";
    return a;
}
void make_path(const std::string& home, const std::string& dir, std::vector<std::string>& made, std::vector<door::Action>& out) {
    if (dir.rfind(home + "/", 0) != 0) return;
    const std::string rest = dir.substr(home.size() + 1);
    for (std::size_t pos = 0;;) {
        const auto s = rest.find('/', pos);
        const std::string cur = home + "/" + rest.substr(0, s);
        if (!exists(cur) && std::find(made.begin(), made.end(), cur) == made.end()) {
            made.push_back(cur);
            out.push_back(make_dir(cur));
        }
        if (s == std::string::npos) break;
        pos = s + 1;
    }
}

}  // namespace

// ── the book ────────────────────────────────────────────────────────────────
std::string to_json(const Book& b) {
    json rs = json::array();
    for (const auto& r : b.rules)
        rs.push_back(json{{"name", r.name}, {"folder", r.folder}, {"glob", r.glob}, {"exts", r.exts},
                          {"older_days", r.older_days}, {"do", do_name(r.act)}, {"dest", r.dest},
                          {"rename", r.rename}, {"enabled", r.enabled}});
    return json{{"watch", b.watch}, {"rules", rs}}.dump(2) + "\n";
}

Book from_json(const std::string& text) {
    Book b;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return b;
    b.watch = j.value("watch", false);
    if (!j.contains("rules") || !j["rules"].is_array()) return b;
    for (const auto& e : j["rules"]) {
        if (!e.is_object()) continue;
        Rule r;
        r.name = e.value("name", "");
        r.folder = e.value("folder", "Downloads");
        r.glob = e.value("glob", "");
        if (e.contains("exts") && e["exts"].is_array())
            for (const auto& x : e["exts"]) if (x.is_string() && !x.get<std::string>().empty()) r.exts.push_back(lower(x.get<std::string>()));
        r.older_days = std::max(0, e.value("older_days", 0));
        r.act = do_of(e.value("do", "move"));
        r.dest = e.value("dest", "");
        r.rename = e.value("rename", "");
        r.enabled = e.value("enabled", false);
        b.rules.push_back(std::move(r));
    }
    return b;
}

Book load(const std::string& file) {
    std::ifstream in(file);
    if (!in) return {};
    std::ostringstream o;
    o << in.rdbuf();
    return from_json(o.str());
}

std::vector<Rule> starters() {
    std::vector<Rule> v;
    Rule a;
    a.name = "Old installers to the Trash";
    a.exts = {"iso", "img", "dmg", "exe", "msi", "deb", "rpm", "appimage", "flatpakref", "run"};
    a.older_days = 30;
    a.act = Do::Trash;
    v.push_back(a);
    Rule b;
    b.name = "PDFs into Documents/PDFs";
    b.exts = {"pdf"};
    b.older_days = 7;
    b.act = Do::Move;
    b.dest = "Documents/PDFs";
    v.push_back(b);
    Rule c;
    c.name = "Fonts into Documents/Fonts";
    c.exts = {"ttf", "otf", "woff", "woff2", "ttc"};
    c.act = Do::Move;
    c.dest = "Documents/Fonts";
    v.push_back(c);
    Rule d;
    d.name = "Screenshots, dated";
    d.folder = "Pictures/Screenshots";
    d.glob = "Screenshot*";
    d.act = Do::Move;
    d.dest = "Pictures/Screenshots/{year}";
    d.rename = "{yyyymmdd-hhmm}_{stem}.{ext}";
    v.push_back(d);
    Rule e;
    e.name = "Untouched downloads to the Archive";
    e.older_days = 180;
    e.act = Do::Archive;
    v.push_back(e);
    return v;
}

// ── matching ────────────────────────────────────────────────────────────────
bool glob_match(const std::string& pattern, const std::string& name) {
    const std::string p = lower(pattern), n = lower(name);
    std::size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
    while (ni < n.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ++pi; ++ni; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool in_progress(const std::string& name) {
    static const std::vector<std::string> ends = {".part", ".crdownload", ".download", ".opdownload", ".partial",
                                                  ".!qb", ".tmp", ".aria2", ".filepart"};
    const std::string n = lower(name);
    for (const auto& e : ends)
        if (n.size() > e.size() && n.compare(n.size() - e.size(), e.size(), e) == 0) return true;
    return false;
}

std::string summary(const Rule& r) {
    std::string what;
    if (!r.glob.empty()) what = "“" + r.glob + "”";
    if (!r.exts.empty()) {
        std::string k;
        for (const auto& e : r.exts) k += (k.empty() ? "" : " ") + ("." + e);
        what += (what.empty() ? "" : ", ") + k;
    }
    if (what.empty()) what = "anything";
    if (r.older_days) what += " untouched " + std::to_string(r.older_days) + (r.older_days == 1 ? " day" : " days");
    std::string act;
    switch (r.act) {
        case Do::Move: act = "move to ~/" + r.dest + (r.rename.empty() ? "" : " as " + r.rename); break;
        case Do::Trash: act = "move to the Trash"; break;
        case Do::Archive: act = "archive (~/Archive/<year>/...)"; break;
        case Do::File: act = "file into ~/" + r.dest; break;
    }
    return "In ~/" + r.folder + ": " + what + " → " + act;
}

std::string problem(const Rule& r, const std::string& home) {
    if (r.name.empty()) return "it needs a name";
    if (!clean_rel(r.folder)) return "the watched folder must be a folder under home";
    if (!is_dir(home + "/" + r.folder)) return "~/" + r.folder + " isn't there";
    if (r.glob.empty() && r.exts.empty() && r.older_days <= 0) return "give it a name, a kind or an age to match -- never everything";
    if (r.act == Do::Move || r.act == Do::File) {
        if (!clean_rel(r.dest)) return "the destination must be a folder under home";
        if (r.dest == r.folder) return "the destination is the watched folder itself";
    }
    if (r.act == Do::Move && r.rename.find('{') != std::string::npos && r.rename.find("{stem}") == std::string::npos &&
        r.rename.find("{yyyymmdd-hhmm}") == std::string::npos)
        return "a rename needs {stem} or {yyyymmdd-hhmm}, or names collide";
    if (r.act == Do::File) {
        organize::Project p;
        if (!organize::load_project(home + "/" + r.dest, p)) return "~/" + r.dest + " is not a project (no .rouster.json)";
    }
    return {};
}

std::vector<Match> matches(const std::string& home, const std::vector<Rule>& rules, std::int64_t now,
                           std::optional<std::size_t> only, std::size_t limit) {
    std::vector<Match> out;
    std::vector<std::size_t> active;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        if (only ? *only != i : (!rules[i].enabled || !problem(rules[i], home).empty())) continue;
        if (only && !problem(rules[i], home).empty()) continue;
        active.push_back(i);
    }
    std::set<std::string> folders, taken;
    for (auto i : active) folders.insert(rules[i].folder);
    for (const auto& f : folders) {
        const std::string dir = home + "/" + f;
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            if (out.size() >= limit) return out;
            if (name.empty() || name[0] == '.' || in_progress(name)) continue;
            const std::string from = dir + "/" + name;
            struct stat st{};
            if (::lstat(from.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            if (now - static_cast<std::int64_t>(st.st_mtim.tv_sec) < kSettle) continue;   // still arriving
            for (auto i : active) {
                const Rule& r = rules[i];
                if (r.folder != f) continue;
                if (!r.glob.empty() && !glob_match(r.glob, name)) continue;
                if (!r.exts.empty() && std::find(r.exts.begin(), r.exts.end(), ext_of(name)) == r.exts.end()) continue;
                if (r.older_days > 0 && now - static_cast<std::int64_t>(st.st_mtim.tv_sec) < std::int64_t(r.older_days) * 86400) continue;
                Match m;
                m.rule = i;
                m.from = from;
                m.bytes = static_cast<std::uint64_t>(st.st_size);
                m.dev = st.st_dev;
                m.ino = st.st_ino;
                m.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
                const std::int64_t mt = st.st_mtim.tv_sec;
                switch (r.act) {
                    case Do::Trash: break;
                    case Do::Archive: {
                        m.to = home + "/" + archive::destination(f + "/" + name, year_of(mt));
                        if (exists(m.to) || taken.count(m.to)) { m.note = "its place in the archive is taken"; m.checked = false; m.to.clear(); }
                        break;
                    }
                    case Do::Move: {
                        const std::string ddir = home + "/" + organize::fill(r.dest, organize::Values{"", "", mt});
                        const std::string base = renamed(r, name, mt);
                        for (int n = 1; n < 1000; ++n) {
                            m.to = ddir + "/" + with_n(base, n);
                            if (!exists(m.to) && !taken.count(m.to)) break;
                        }
                        break;
                    }
                    case Do::File: {
                        organize::Project p;
                        organize::load_project(home + "/" + r.dest, p);
                        const auto in = organize::propose(home, p, {from});
                        if (in.empty() || in[0].to.empty()) { m.note = in.empty() ? "can't be filed" : in[0].note; m.checked = false; }
                        else {
                            m.to = in[0].to;
                            for (int n = 2; taken.count(m.to) && n < 1000; ++n) m.to = with_n(in[0].to, n);
                        }
                        break;
                    }
                }
                if (!m.to.empty()) taken.insert(m.to);
                out.push_back(std::move(m));
                break;   // the first rule that matches wins
            }
        }
    }
    return out;
}

door::Plan plan(const std::string& home, const std::vector<Rule>& rules, const std::vector<Match>& ms,
                std::vector<std::string>& dropped) {
    door::Plan plan;
    std::vector<door::Action> acts;
    std::vector<std::string> made;
    std::size_t n = 0;
    for (const auto& m : ms) {
        if (!m.checked || m.rule >= rules.size()) continue;
        const Rule& r = rules[m.rule];
        struct stat st{};
        if (::lstat(m.from.c_str(), &st) != 0) { dropped.push_back(m.from + " -- gone since"); continue; }
        const std::int64_t mt = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        if (st.st_dev != m.dev || st.st_ino != m.ino || mt != m.mtime_ns) { dropped.push_back(m.from + " -- changed since"); continue; }
        door::Action a;
        a.from = m.from;
        a.bytes = m.bytes;
        a.reason = "rule “" + r.name + "”";
        if (r.act == Do::Trash) a.verb = door::Verb::Trash;
        else {
            if (m.to.empty()) continue;
            if (exists(m.to)) { dropped.push_back(m.to + " -- that place is taken now"); continue; }
            make_path(home, fs::path(m.to).parent_path().string(), made, plan.actions);
            a.verb = door::Verb::Move;
            a.to = m.to;
        }
        acts.push_back(a);
        ++n;
    }
    for (auto& a : acts) plan.actions.push_back(std::move(a));
    plan.label = "Rules: " + std::to_string(n) + (n == 1 ? " file" : " files");
    return plan;
}

}  // namespace rouster::core::rules
