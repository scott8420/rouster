#include "core/Tidy.hpp"
#include "core/Catalog.hpp"
#include "core/Scan.hpp"   // human_bytes

#include <algorithm>
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
    "SELECT path, coalesce(size, 0), " TIDY_LU ", package FROM files "                           \
    "WHERE gone IS NULL AND id NOT IN (SELECT file_id FROM decisions d WHERE file_id IS NOT NULL " \
    "AND verdict = 'keep-here' AND d.id = (SELECT max(id) FROM decisions WHERE file_id = d.file_id)) AND "   \
    "path NOT GLOB 'Develop/*' AND path NOT GLOB 'Projects/*' AND path NOT GLOB 'src/*' AND "          \
    "path NOT GLOB 'dev/*' AND path NOT GLOB 'code/*' AND "

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
        case Verdict::Short:     return "Useful once, done now -- they would wait in the Closet, then go to the Trash at a review.";
        case Verdict::Dormant:   return "Neither opened nor changed in 18 months -- kept, finished: they would mature to the Archive.";
        case Verdict::Undecided: return "Loose at the top of home or on the Desktop -- no home yet: they would go to the Closet to be sorted.";
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
            g.rows.push_back({row[0], b, i(row[2]), q.id, row[3] == "1"});
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

    struct Acc { std::uint64_t files = 0, bytes = 0, arrived = 0, unread = 0; std::set<std::string> kinds; };
    std::map<std::string, Acc> folders, tops;
    for (const auto& row : all.rows) {
        const std::uint64_t b = u(row[1]);
        ++r.files;
        r.bytes += b;
        if (!claimed.count(row[0])) { ++r.active; r.active_bytes += b; }
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
        if (f.score >= 3.0 && a.files >= 3) r.folders.push_back(std::move(f));
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
            if (n == "Archive") t.note = "the Archive";
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
        if (key == "DOWNLOAD" && rel != "Closet") r.drift.push_back("Downloads land in ~/" + rel + " (the Closet will take them -- s016)");
    }
    if (!is_dir(home + "/Closet")) r.drift.push_back("No ~/Closet yet -- the landing place for new files (s016 makes it)");

    r.ok = true;
    return r;
}

}  // namespace rouster::core::tidy
