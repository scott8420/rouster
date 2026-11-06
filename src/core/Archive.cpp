#include "core/Archive.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <set>

#include <dirent.h>
#include <sys/stat.h>

// Archive.cpp -- READ-ONLY: opendir + lstat. The door's MakeDir and Move act.

namespace rouster::core::archive {

namespace fs = std::filesystem;

const std::vector<std::string>& dev_roots() {
    static const std::vector<std::string> v = {"Develop", "Projects", "src", "dev", "code"};
    return v;
}

std::string destination(const std::string& rel, int year) {
    return std::string(kRoot) + "/" + std::to_string(year) + "/" + rel;
}

namespace {

int year_of(std::int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    return tm.tm_year + 1900;
}

std::int64_t ns(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

bool has(const std::string& abs, const char* name) {
    struct stat st{};
    return ::lstat((abs + "/" + name).c_str(), &st) == 0;
}

struct Walk {
    std::string home;
    dev_t dev = 0;
    std::int64_t cutoff = 0;
    scan::Progress* prog = nullptr;
    Found* out = nullptr;

    struct Sum {
        bool clean = true, all_old = true;
        std::uint64_t bytes = 0, files = 0;
        std::int64_t newest = 0;
    };

    // Walks `rel` (a folder at depth >= 1). Old files / old folders go into
    // out->items; returns whether the folder as a whole could be one item.
    Sum dir(const std::string& rel, scan::Kind parent, int depth) {
        Sum sum;
        const std::string abs = home + "/" + rel;
        DIR* d = ::opendir(abs.c_str());
        if (!d) { ++out->errors; sum.clean = false; return sum; }
        std::vector<std::string> names;
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") names.push_back(n);
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        ++prog->dirs;
        const std::size_t start = out->items.size();
        for (const auto& n : names) {
            if (prog->cancel) { sum.clean = false; break; }
            if (n[0] == '.') { sum.clean = false; continue; }
            const std::string crel = rel + "/" + n;
            const std::string cabs = home + "/" + crel;
            struct stat st{};
            if (::lstat(cabs.c_str(), &st) != 0) { ++out->errors; sum.clean = false; continue; }
            if (S_ISDIR(st.st_mode)) {
                if (st.st_dev != dev || n == "node_modules" || has(cabs, ".git")) { sum.clean = false; continue; }
                const bool build = has(cabs, "CMakeCache.txt") || has(cabs, "build.ninja") || has(cabs, "meson-info");
                const scan::Kind k = scan::classify_dir(crel, parent, build).kind;
                if (k != scan::Kind::Other) { sum.clean = false; continue; }   // Protected, caches, builds, ...
                const Sum c = dir(crel, k, depth + 1);
                sum.clean = sum.clean && c.clean;
                sum.all_old = sum.all_old && c.all_old;
                sum.bytes += c.bytes;
                sum.files += c.files;
                sum.newest = std::max(sum.newest, c.newest);
            } else if (S_ISREG(st.st_mode)) {
                ++prog->files;
                const std::uint64_t b = static_cast<std::uint64_t>(st.st_blocks) * 512u;
                prog->bytes += b;
                sum.bytes += b;
                ++sum.files;
                sum.newest = std::max<std::int64_t>(sum.newest, st.st_mtim.tv_sec);
                if (st.st_mtim.tv_sec >= cutoff) { sum.all_old = false; continue; }
                Item it;
                it.rel = crel;
                it.bytes = b;
                it.newest = st.st_mtim.tv_sec;
                it.year = year_of(it.newest);
                it.dev = st.st_dev; it.ino = st.st_ino; it.mtime_ns = ns(st);
                out->items.push_back(it);
            } else {
                sum.clean = false;   // symlink, fifo, socket, device: stays, and its folder isn't whole
            }
        }
        // The whole folder is one item: replace what was found inside it.
        if (depth >= 2 && sum.clean && sum.all_old && sum.files > 0) {
            struct stat st{};
            if (::lstat(abs.c_str(), &st) == 0) {
                out->items.resize(start);
                Item it;
                it.rel = rel;
                it.folder = true;
                it.bytes = sum.bytes;
                it.files = sum.files;
                it.newest = sum.newest;
                it.year = year_of(sum.newest);
                it.dev = st.st_dev; it.ino = st.st_ino;
                out->items.push_back(it);
            }
        }
        return sum;
    }
};

// The card an item sits under: its top folder, or "" for a loose file at the top of home.
std::string top_of(const std::string& rel) {
    const auto s = rel.find('/');
    return s == std::string::npos ? std::string() : rel.substr(0, s);
}

// MakeDir actions for every missing folder from home/<first> down to `dir`
// (absolute), parents first, each only once in the plan.
void need_dirs(const std::string& home, const std::string& dir, std::set<std::string>& planned,
               std::vector<door::Action>& out) {
    const std::string rel = dir.substr(home.size() + 1);
    std::size_t pos = 0;
    while (true) {
        const auto s = rel.find('/', pos);
        const std::string abs = home + "/" + rel.substr(0, s);
        struct stat st{};
        if (::lstat(abs.c_str(), &st) != 0 && planned.insert(abs).second) {
            door::Action a;
            a.verb = door::Verb::MakeDir;
            a.to = abs;
            a.reason = "a folder in the archive";
            out.push_back(a);
        }
        if (s == std::string::npos) break;
        pos = s + 1;
    }
}

}  // namespace

Found find(const std::string& home_in, std::int64_t now, int months, scan::Progress& prog) {
    Found out;
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    struct stat hs{};
    if (home.empty() || home == "/" || ::lstat(home.c_str(), &hs) != 0 || !S_ISDIR(hs.st_mode)) {
        out.refused = "not a home folder: " + home_in;
        prog.done = true;
        return out;
    }
    Walk w;
    w.home = home;
    w.dev = hs.st_dev;
    w.cutoff = now - static_cast<std::int64_t>(months) * 2629746;   // a Gregorian month in seconds
    w.prog = &prog;
    w.out = &out;

    std::vector<std::string> names;
    if (DIR* d = ::opendir(home.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != ".." && n[0] != '.') names.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(names.begin(), names.end());
    const auto& dev = dev_roots();
    for (const auto& n : names) {
        if (prog.cancel) break;
        if (n == kRoot || std::find(dev.begin(), dev.end(), n) != dev.end()) continue;
        const std::string abs = home + "/" + n;
        struct stat st{};
        if (::lstat(abs.c_str(), &st) != 0) { ++out.errors; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != hs.st_dev || has(abs, ".git")) continue;
            const scan::Kind k = scan::classify_dir(n, scan::Kind::Other, false).kind;
            if (k != scan::Kind::Other) continue;
            w.dir(n, k, 1);
        } else if (S_ISREG(st.st_mode)) {
            ++prog.files;
            if (st.st_mtim.tv_sec >= w.cutoff) continue;
            if (scan::classify_file(n, scan::Kind::Other) != scan::Kind::Other) continue;
            Item it;
            it.rel = n;
            it.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
            it.newest = st.st_mtim.tv_sec;
            it.year = year_of(it.newest);
            it.dev = st.st_dev; it.ino = st.st_ino; it.mtime_ns = ns(st);
            out.items.push_back(it);
        }
    }
    out.files = prog.files;
    for (const auto& it : out.items) out.old_bytes += it.bytes;
    std::stable_sort(out.items.begin(), out.items.end(), [](const Item& a, const Item& b) {
        const auto ta = top_of(a.rel), tb = top_of(b.rel);
        if (ta != tb) return ta < tb;
        return a.bytes > b.bytes;
    });
    out.cancelled = prog.cancel;
    prog.done = true;
    return out;
}

door::Plan plan(const std::string& home_in, const std::vector<Item>& items, std::vector<std::string>& dropped) {
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    door::Plan p;
    std::set<std::string> planned;
    std::vector<door::Action> dirs, moves;
    for (const auto& it : items) {
        if (!it.checked) continue;
        const std::string from = home + "/" + it.rel;
        struct stat st{};
        if (::lstat(from.c_str(), &st) != 0 || st.st_dev != it.dev || st.st_ino != it.ino ||
            (!it.folder && ns(st) != it.mtime_ns)) {
            dropped.push_back("~/" + it.rel + " changed since Look -- left out");
            continue;
        }
        const std::string to = home + "/" + destination(it.rel, it.year);
        if (::lstat(to.c_str(), &st) == 0 || planned.count(to)) {
            dropped.push_back("~/" + destination(it.rel, it.year) + " is taken -- left out");
            continue;
        }
        need_dirs(home, fs::path(to).parent_path().string(), planned, dirs);
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = from;
        a.to = to;
        a.bytes = it.bytes;
        a.reason = "untouched since " + std::to_string(it.year);
        moves.push_back(a);
    }
    p.actions = dirs;
    p.actions.insert(p.actions.end(), moves.begin(), moves.end());
    p.label = "Archive: " + std::to_string(moves.size()) + (moves.size() == 1 ? " item" : " items");
    return p;
}

std::vector<Stored> stored(const door::Door& door, const std::string& home_in) {
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    const std::string root = home + "/" + kRoot + "/";
    std::vector<Stored> out;
    std::set<std::string> seen;
    const auto& bs = door.batches();
    for (auto b = bs.rbegin(); b != bs.rend(); ++b) {   // newest first: a path archived twice shows once
        if (b->undone) continue;
        for (auto id : b->records) {
            const door::Record* r = door.record(id);
            if (!r || r->verb != door::Verb::Move || r->state != door::State::Done) continue;
            if (r->to.rfind(root, 0) != 0 || r->from.rfind(home + "/", 0) != 0) continue;
            door::Identity now{};
            if (!door::identity_of(r->to, now) || now.dev != r->ident.dev || now.ino != r->ident.ino) continue;
            if (!seen.insert(r->to).second) continue;
            Stored s;
            s.rel = r->to.substr(home.size() + 1);
            s.back = r->from.substr(home.size() + 1);
            s.folder = now.dir;
            s.when = r->when;
            struct stat st{};
            s.blocked = ::lstat(r->from.c_str(), &st) == 0;
            if (!s.folder) {
                if (::lstat(r->to.c_str(), &st) == 0) s.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
            } else {
                for (auto it = fs::recursive_directory_iterator(r->to, ec); !ec && it != fs::recursive_directory_iterator();
                     it.increment(ec))
                    if (::lstat(it->path().c_str(), &st) == 0) s.bytes += static_cast<std::uint64_t>(st.st_blocks) * 512u;
            }
            out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end(), [](const Stored& a, const Stored& b) { return a.rel < b.rel; });
    return out;
}

door::Plan put_back(const std::string& home_in, const std::vector<Stored>& items, std::vector<std::string>& dropped) {
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    door::Plan p;
    std::set<std::string> planned;
    std::vector<door::Action> dirs, moves;
    for (const auto& s : items) {
        if (!s.checked) continue;
        const std::string to = home + "/" + s.back;
        struct stat st{};
        if (::lstat(to.c_str(), &st) == 0 || planned.count(to)) {
            dropped.push_back("~/" + s.back + " is taken -- left in the archive");
            continue;
        }
        planned.insert(to);
        need_dirs(home, fs::path(to).parent_path().string(), planned, dirs);
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = home + "/" + s.rel;
        a.to = to;
        a.bytes = s.bytes;
        a.reason = "back where it was";
        moves.push_back(a);
    }
    p.actions = dirs;
    p.actions.insert(p.actions.end(), moves.begin(), moves.end());
    p.label = "Archive: put back " + std::to_string(moves.size()) + (moves.size() == 1 ? " item" : " items");
    return p;
}

}  // namespace rouster::core::archive
