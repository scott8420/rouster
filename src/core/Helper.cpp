#include "core/Helper.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include <dirent.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>

namespace fs = std::filesystem;

namespace rouster::core::helper {

const char* op_name(Op o) {
    switch (o) {
        case Op::Snapshot: return "snapshot";
    }
    return "?";
}

std::optional<Request> parse(const std::string& s) {
    const auto c = s.find(':');
    if (c == std::string::npos || s.find(':', c + 1) != std::string::npos) return std::nullopt;
    const std::string word = s.substr(0, c), num = s.substr(c + 1);
    if (num.empty() || num.size() > 3 || !std::all_of(num.begin(), num.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return std::nullopt;
    const int n = std::stoi(num);
    if (word == op_name(Op::Snapshot)) {
        if (n < kKeepMin || n > kKeepMax) return std::nullopt;
        return Request{Op::Snapshot, n};
    }
    return std::nullopt;
}

std::string text(const Request& r) { return std::string(op_name(r.op)) + ":" + std::to_string(r.arg); }

std::string describe(const Request& r) {
    switch (r.op) {
        case Op::Snapshot:
            return "Read-only snapshot of home's subvolume (keeps Rouster's newest " + std::to_string(r.arg) + ")";
    }
    return "?";
}

std::string snapshot_name(std::time_t t) {
    std::tm tm{};
    ::gmtime_r(&t, &tm);   // UTC: pkexec clears TZ, so the helper and the app could disagree on "local"
    char buf[32];
    std::strftime(buf, sizeof buf, "home-%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool is_snapshot_name(const std::string& s) {
    // home-YYYYMMDD-HHMMSS, exactly.
    if (s.size() != 20 || s.rfind("home-", 0) != 0 || s[13] != '-') return false;
    for (std::size_t i = 5; i < 20; ++i)
        if (i != 13 && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}

std::optional<std::time_t> snapshot_time(const std::string& name) {
    if (!is_snapshot_name(name)) return std::nullopt;
    std::tm tm{};
    tm.tm_year = std::stoi(name.substr(5, 4)) - 1900;
    tm.tm_mon = std::stoi(name.substr(9, 2)) - 1;
    tm.tm_mday = std::stoi(name.substr(11, 2));
    tm.tm_hour = std::stoi(name.substr(14, 2));
    tm.tm_min = std::stoi(name.substr(16, 2));
    tm.tm_sec = std::stoi(name.substr(18, 2));
    const std::time_t t = ::timegm(&tm);
    if (t == static_cast<std::time_t>(-1)) return std::nullopt;
    return t;
}

std::vector<std::string> to_expire(std::vector<std::string> names, int keep) {
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return !is_snapshot_name(n); }),
                names.end());
    std::sort(names.begin(), names.end(), std::greater<>());   // newest first (the stamp sorts)
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (keep < kKeepMin) keep = kKeepMin;
    if (names.size() <= static_cast<std::size_t>(keep)) return {};
    return {names.begin() + keep, names.end()};
}

Place place_for(const std::string& home) {
    Place p;
    char* real = ::realpath(home.c_str(), nullptr);
    if (!real) { p.why = "can't read " + home; return p; }
    const std::string h = real;
    std::free(real);
    struct statfs sf{};
    if (::statfs(h.c_str(), &sf) != 0) { p.why = "can't read " + h; return p; }
    if (static_cast<unsigned long>(sf.f_type) != static_cast<unsigned long>(BTRFS_SUPER_MAGIC)) {
        p.why = "home is not on btrfs";
        return p;
    }
    struct stat hs{};
    if (::stat(h.c_str(), &hs) != 0) { p.why = "can't read " + h; return p; }
    // Each btrfs subvolume (and each mount) has its own st_dev: climb while the
    // parent is still on home's -- the last one is the subvolume's top folder.
    fs::path cur = h;
    while (cur != cur.root_path()) {
        const fs::path up = cur.parent_path();
        struct stat us{};
        if (::stat(up.c_str(), &us) != 0 || us.st_dev != hs.st_dev) break;
        cur = up;
    }
    struct stat cs{};
    if (::stat(cur.c_str(), &cs) != 0) { p.why = "can't read " + cur.string(); return p; }
    if (cur == cur.root_path()) {
        p.why = "home shares the subvolume mounted at / -- a snapshot would take the whole system";
        return p;
    }
    if (cs.st_ino != 256) {   // BTRFS_FIRST_FREE_OBJECTID: a subvolume's top folder
        p.why = cur.string() + " is not the top of a subvolume";
        return p;
    }
    p.ok = true;
    p.subvol = cur.string();
    p.dir = p.subvol + "/" + kSnapDir;
    return p;
}

std::vector<std::string> list_snapshots(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (const dirent* e = ::readdir(d))
        if (is_snapshot_name(e->d_name)) out.emplace_back(e->d_name);
    ::closedir(d);
    std::sort(out.begin(), out.end(), std::greater<>());
    return out;
}

std::string format_line(const Line& l) {
    const auto flat = [](std::string s) {
        for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    return "rouster-helper\t" + flat(l.op) + "\t" + flat(l.status) + "\t" + flat(l.detail);
}

std::optional<Line> parse_line(const std::string& s) {
    std::vector<std::string> f;
    std::size_t pos = 0;
    while (f.size() < 3) {
        const auto t = s.find('\t', pos);
        if (t == std::string::npos) return std::nullopt;
        f.push_back(s.substr(pos, t - pos));
        pos = t + 1;
    }
    if (f[0] != "rouster-helper") return std::nullopt;
    Line l{f[1], f[2], s.substr(pos)};
    if (l.status != "ok" && l.status != "failed" && l.status != "dry") return std::nullopt;
    return l;
}

std::vector<Line> parse_output(const std::string& out) {
    std::vector<Line> v;
    std::istringstream in(out);
    for (std::string l; std::getline(in, l);)
        if (auto x = parse_line(l)) v.push_back(*x);
    return v;
}

}  // namespace rouster::core::helper
