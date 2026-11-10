#include "core/Scan.hpp"
#include "core/Package.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>

#include <cerrno>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Scan.cpp -- the read-only walker. Every syscall in this file is a read:
// open(O_RDONLY|O_DIRECTORY), readdir, fstatat(AT_SYMLINK_NOFOLLOW). If a write
// ever appears here, it is a bug (RULES: read-only until the s004 door).

namespace rouster::core::scan {

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Regenerable: return "Regenerable";
        case Kind::State:       return "State";
        case Kind::Settings:    return "Settings";
        case Kind::AppData:     return "App data";
        case Kind::Trash:       return "Trash";
        case Kind::Protected:   return "Protected";
        case Kind::Other:       return "Your files";
    }
    return "?";
}

const char* kind_treatment(Kind k) {
    switch (k) {
        case Kind::Regenerable: return "Caches and build output -- rebuilt on demand. Safe to clean.";
        case Kind::State:       return "Logs, histories, app state -- mostly safe; decided item by item.";
        case Kind::Settings:    return "Your app settings -- backed up, never cleaned; orphans only flagged.";
        case Kind::AppData:     return "Data apps keep for you -- backed up, never cleaned automatically.";
        case Kind::Trash:       return "Already in the Trash -- emptying it is a choice you make.";
        case Kind::Protected:   return "Keys, keyrings, desktop settings, fixes/ -- Rouster never touches these.";
        case Kind::Other:       return "Your own files -- organized, never cleaned without you choosing.";
    }
    return "";
}

const std::vector<std::string>& never_touch() {
    static const std::vector<std::string> list = {
        ".local/share/keyrings",
        ".config/dconf",
        ".ssh",
        ".gnupg",
        ".local/share/gnome-shell/extensions",
        ".local/share/flatpak",
    };
    return list;
}

namespace {
// std::atomic<shared_ptr> (C++20): the free functions on shared_ptr are deprecated (GCC 16).
std::atomic<std::shared_ptr<const std::vector<std::string>>> g_owned{std::make_shared<const std::vector<std::string>>()};
}  // namespace

void set_owned(std::vector<std::string> rels) {
    g_owned.store(std::make_shared<const std::vector<std::string>>(std::move(rels)));
}
std::vector<std::string> owned() { return *g_owned.load(); }

bool protected_name(std::string_view n) {
    static constexpr std::string_view ext = ".samson";
    if (n.size() <= ext.size()) return false;
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(n[n.size() - ext.size() + i])) != ext[i]) return false;
    return true;
}

std::vector<std::string> samson_places(const std::string& home, const std::string& config_home) {
    std::string samson, catalog, second;
    {
        std::ifstream in(config_home + "/samson/settings.json");
        if (in) {
            const auto j = nlohmann::json::parse(in, nullptr, false);
            const auto get = [&](const char* k, std::string& out) {
                if (j.is_object() && j.contains(k) && j[k].is_string()) out = j[k].get<std::string>();
            };
            get("samson_dir", samson);
            get("catalog_dir", catalog);
            get("second_home", second);
        }
    }
    if (samson.empty()) samson = home + "/Samson";
    if (catalog.empty()) catalog = home + "/.local/share/samson";
    std::vector<std::string> out;
    for (std::string p : {samson, catalog, second}) {
        if (p.empty()) continue;
        if (p.rfind("~/", 0) == 0) p = home + p.substr(1);
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p.size() <= home.size() + 1 || p.compare(0, home.size() + 1, home + "/") != 0) continue;
        const std::string rel = p.substr(home.size() + 1);
        if (rel.find("/../") != std::string::npos || rel.rfind("../", 0) == 0 || rel == "..") continue;
        out.push_back(rel);
    }
    return out;
}

namespace {

std::string basename_of(const std::string& rel) {
    const auto s = rel.rfind('/');
    return s == std::string::npos ? rel : rel.substr(s + 1);
}

// ".var/app/<id>/<leaf>" -> leaf, else "".
std::string flatpak_leaf(const std::string& rel) {
    static const std::string pre = ".var/app/";
    if (rel.rfind(pre, 0) != 0) return {};
    const std::string rest = rel.substr(pre.size());
    const auto s = rest.find('/');
    if (s == std::string::npos || s == 0) return {};
    const std::string leaf = rest.substr(s + 1);
    return leaf.find('/') == std::string::npos ? leaf : std::string{};
}

}  // namespace

DirClass classify_dir(const std::string& rel, Kind parent, bool looks_like_build) {
    // Protected wins over everything, and is inherited all the way down.
    const auto& nt = never_touch();
    if (std::find(nt.begin(), nt.end(), rel) != nt.end() || basename_of(rel) == "fixes")
        return {Kind::Protected, true, false};
    {   // s017a2: Samson's places; a Samson container wherever it is
        const auto own = g_owned.load();
        if (std::find(own->begin(), own->end(), rel) != own->end() || protected_name(basename_of(rel)))
            return {Kind::Protected, true, false};
    }
    if (parent == Kind::Protected) return {Kind::Protected, false, false};
    if (parent == Kind::Trash)     return {Kind::Trash, false, false};

    // s015a: a Mac package (and all of it) is your files, wherever it sits --
    // never a cache, a build tree or app data to be swept.
    if (!pkg::package_of(rel, true).empty()) return {Kind::Other, false, false};

    if (rel == ".local/share/Trash") return {Kind::Trash, true, false};
    if (rel == ".cache")             return {Kind::Regenerable, true, true};
    if (rel == ".config")            return {Kind::Settings, true, true};
    if (rel == ".local/state")       return {Kind::State, true, true};
    if (rel == ".local/share")       return {Kind::AppData, true, true};
    if (rel == ".var/app")           return {Kind::AppData, true, true};

    const std::string leaf = flatpak_leaf(rel);
    if (leaf == "cache")  return {Kind::Regenerable, true, false};
    if (leaf == "config") return {Kind::Settings, true, false};
    if (leaf == "data")   return {Kind::AppData, true, false};

    // A build tree in the user's own files: only when it carries a generator's
    // marker, so a folder that merely happens to be called "build" stays Other.
    if (parent == Kind::Other && looks_like_build && basename_of(rel) == "build")
        return {Kind::Regenerable, true, false};

    return {parent, false, false};
}

Kind classify_file(const std::string& rel, Kind parent) {
    if (parent != Kind::Other) return parent;
    if (protected_name(basename_of(rel))) return Kind::Protected;   // s017a2: a Samson container
    static const std::set<std::string> histories = {
        ".bash_history", ".zsh_history", ".python_history", ".lesshst",
        ".node_repl_history", ".wget-hsts", ".sqlite_history", ".xsession-errors",
    };
    if (rel.find('/') == std::string::npos && histories.count(rel)) return Kind::State;
    return parent;
}

std::uint64_t Report::total_bytes() const {
    std::uint64_t t = 0;
    for (const auto& b : by_kind) t += b.bytes;
    return t;
}

std::string human_bytes(std::uint64_t b) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1000.0 && u < 5) { v /= 1000.0; ++u; }
    char buf[32];
    if (u == 0)        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
    else if (v < 10.0) std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    else               std::snprintf(buf, sizeof buf, "%.0f %s", v, units[u]);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
// The walker.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct Task {
    std::string rel;        // relative to home ("" = home)
    Kind        kind;
    std::string item;       // the Item its direct files are charged to ("" = none)
    bool        expand;     // its children become separate items
    std::string pkg;        // s015a: the package it is (or is inside), "" = none
    bool        cat = true; // s015b: its files go in the catalog (your files, not hidden / a repo / node_modules)
};

struct Newest { std::int64_t change = 0, read = 0; };

struct Local {               // one worker's tallies, merged at the end
    std::array<Bucket, kKindCount>   by_kind{};
    std::map<std::string, Item>      items;
    std::map<std::string, Item>      pkgs;      // s015a: each package, whole
    std::map<std::string, Newest>    pkg_new;   // s015b: each package's newest change / read
    std::vector<scan::Seen>          mine;      // s015b: your files, for the catalog
    std::vector<std::string>         skipped;
    std::vector<std::string>         unwalked;
    std::uint64_t errors = 0, dirs = 0;

    void charge(Kind k, const std::string& item, std::uint64_t bytes, bool is_file) {
        auto& b = by_kind[static_cast<std::size_t>(k)];
        b.bytes += bytes;
        if (is_file) ++b.files;
        if (item.empty() || k == Kind::Other) return;
        auto [it, fresh] = items.try_emplace(item, Item{item, k, 0, 0});
        it->second.bytes += bytes;
        if (is_file) ++it->second.files;
    }
    void charge_pkg(const std::string& p, std::uint64_t bytes, bool is_file) {
        if (p.empty()) return;
        auto [it, fresh] = pkgs.try_emplace(p, Item{p, Kind::Other, 0, 0});
        it->second.bytes += bytes;
        if (is_file) ++it->second.files;
    }
    void newest_pkg(const std::string& p, std::int64_t change, std::int64_t read) {
        if (p.empty()) return;
        auto& n = pkg_new[p];
        n.change = std::max(n.change, change);
        n.read   = std::max(n.read, read);
    }
};

std::int64_t birth(int dfd, const char* name) {   // statx btime; 0 = unknown
    struct statx sx{};
    if (::statx(dfd, name, AT_SYMLINK_NOFOLLOW | AT_STATX_DONT_SYNC, STATX_BTIME, &sx) == 0 &&
        (sx.stx_mask & STATX_BTIME))
        return static_cast<std::int64_t>(sx.stx_btime.tv_sec);
    return 0;
}

// Opened for reading its entries -- O_NOATIME where the kernel allows it
// (files you own), so Rouster's own look doesn't bump the folder's atime.
int open_dir(const char* abs) {
    int fd = ::open(abs, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOATIME);
    if (fd < 0 && errno == EPERM) fd = ::open(abs, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return fd;
}

std::string join(const std::string& rel, const char* name) {
    return rel.empty() ? std::string(name) : rel + "/" + name;
}

bool has_marker(int dfd, const char* name) {
    struct stat st{};
    for (const char* m : {"/CMakeCache.txt", "/build.ninja", "/meson-info"}) {
        const std::string p = std::string(name) + m;
        if (fstatat(dfd, p.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) return true;
    }
    return false;
}

Task child_task(const Task& parent, const std::string& rel, bool build) {
    const DirClass dc = classify_dir(rel, parent.kind, build);
    Task t{rel, dc.kind, parent.item, false, parent.pkg, parent.cat};
    if (t.pkg.empty() && dc.kind == Kind::Other && pkg::is_package_name(basename_of(rel))) t.pkg = rel;
    const std::string leaf = basename_of(rel);
    if (dc.kind != Kind::Other || !t.pkg.empty() || leaf.empty() || leaf[0] == '.' || leaf == "node_modules")
        t.cat = false;
    if (dc.rooted)          { t.item = rel; t.expand = dc.expand; }
    else if (parent.expand) { t.item = rel; }
    if (t.kind == Kind::Other) t.item.clear();
    return t;
}

class Walker {
public:
    Walker(std::string home, dev_t dev, Progress& p, bool collect)
        : m_home(std::move(home)), m_dev(dev), m_p(p), m_collect(collect) {}

    void push(Task t) {
        std::lock_guard lk(m_mx);
        m_q.push_back(std::move(t));
        ++m_pending;
        m_cv.notify_one();
    }

    void run(Local& out) {
        for (;;) {
            Task t;
            {
                std::unique_lock lk(m_mx);
                m_cv.wait(lk, [&] { return !m_q.empty() || m_pending == 0; });
                if (m_q.empty()) return;          // pending == 0: all done
                t = std::move(m_q.front());
                m_q.pop_front();
            }
            if (!m_p.cancel.load(std::memory_order_relaxed)) visit(t, out);
            {
                std::lock_guard lk(m_mx);
                if (--m_pending == 0) m_cv.notify_all();
            }
        }
    }

private:
    bool first_link(dev_t d, ino_t i) {
        std::lock_guard lk(m_links_mx);
        return m_links.emplace(d, i).second;
    }

    void visit(const Task& t0, Local& out) {
        const std::string abs = t0.rel.empty() ? m_home : m_home + "/" + t0.rel;
        const int dfd = open_dir(abs.c_str());
        if (dfd < 0) { ++out.errors; out.unwalked.push_back(t0.rel); return; }
        DIR* d = ::fdopendir(dfd);
        if (!d) { ::close(dfd); ++out.errors; out.unwalked.push_back(t0.rel); return; }
        Task t = t0;
        if (t.cat) {   // a git repository is Dev projects' -- none of it goes in the catalog
            struct stat g{};
            if (::fstatat(dfd, ".git", &g, AT_SYMLINK_NOFOLLOW) == 0) t.cat = false;
        }
        ++out.dirs;
        m_p.dirs.fetch_add(1, std::memory_order_relaxed);

        while (dirent* e = ::readdir(d)) {
            const char* n = e->d_name;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
            if (m_p.cancel.load(std::memory_order_relaxed)) break;
            struct stat st{};
            if (::fstatat(dfd, n, &st, AT_SYMLINK_NOFOLLOW) != 0) { ++out.errors; continue; }
            const std::string rel = join(t.rel, n);
            const std::uint64_t bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;

            if (S_ISDIR(st.st_mode)) {
                if (st.st_dev != m_dev) { out.skipped.push_back(rel); continue; }
                const bool build = std::string_view(n) == "build" && has_marker(dfd, n);
                Task c = child_task(t, rel, build);
                out.charge(c.kind, c.item, bytes, false);   // the directory's own blocks
                out.charge_pkg(c.pkg, bytes, false);
                out.newest_pkg(c.pkg, st.st_mtim.tv_sec, 0);   // a folder's atime is our own readdir: never used
                if (m_collect && t.cat && t.pkg.empty() && c.pkg == rel)   // a package: ONE entry, filled in after
                    out.mine.push_back({rel, 0, 0, 0, birth(dfd, n), static_cast<std::uint64_t>(st.st_dev),
                                        static_cast<std::uint64_t>(st.st_ino), true});
                m_p.bytes.fetch_add(bytes, std::memory_order_relaxed);
                push(std::move(c));
                continue;
            }
            const Kind k = classify_file(rel, t.kind);
            out.newest_pkg(t.pkg, st.st_mtim.tv_sec, st.st_atim.tv_sec);
            if (m_collect && t.cat && k == Kind::Other && S_ISREG(st.st_mode) && n[0] != '.')
                out.mine.push_back({rel, bytes, st.st_mtim.tv_sec, st.st_atim.tv_sec, birth(dfd, n),
                                    static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino), false});
            if (st.st_nlink > 1 && !first_link(st.st_dev, st.st_ino)) continue;
            out.charge(k, k == t.kind ? t.item : rel, bytes, true);
            out.charge_pkg(t.pkg, bytes, true);
            m_p.files.fetch_add(1, std::memory_order_relaxed);
            m_p.bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ::closedir(d);   // closes dfd
    }

    std::string m_home;
    dev_t       m_dev;
    Progress&   m_p;
    bool        m_collect;

    std::mutex              m_mx;
    std::condition_variable m_cv;
    std::deque<Task>        m_q;
    std::size_t             m_pending = 0;

    std::mutex                        m_links_mx;
    std::set<std::pair<dev_t, ino_t>> m_links;
};

// The root's own task: classify each component from home down, as a full scan
// would have on its way there.
Task root_task(const std::string& home, const std::string& rel) {
    Task t{"", Kind::Other, "", false, "", true};
    if (rel.empty()) return t;
    const auto repo = [](const std::string& dir) {
        struct stat g{};
        return ::lstat((dir + "/.git").c_str(), &g) == 0;
    };
    if (repo(home)) t.cat = false;
    std::string cur;
    std::size_t pos = 0;
    while (pos <= rel.size()) {
        const auto s = rel.find('/', pos);
        const std::string part = rel.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
        const std::string parent_abs = cur.empty() ? home : home + "/" + cur;
        bool build = false;
        if (part == "build") {
            const int dfd = ::open(parent_abs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd >= 0) { build = has_marker(dfd, "build"); ::close(dfd); }
        }
        cur = cur.empty() ? part : cur + "/" + part;
        t = child_task(t, cur, build);
        if (t.cat && repo(home + "/" + cur)) t.cat = false;
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    return t;
}

}  // namespace

Report scan(const std::string& home, const std::string& root, Progress& p, unsigned threads) {
    return scan(home, root, p, threads, nullptr);
}

Report scan(const std::string& home_in, const std::string& root_in,
            Progress& p, unsigned threads, std::vector<Seen>* mine) {
    if (mine) mine->clear();
    namespace fs = std::filesystem;
    Report r;
    std::error_code ec;
    const fs::path home = fs::weakly_canonical(fs::path(home_in), ec);
    const fs::path root = fs::weakly_canonical(fs::path(root_in.empty() ? home_in : root_in), ec);
    r.home = home.string();
    r.root = root.string();

    const auto refuse = [&](std::string why) { r.refused = std::move(why); p.done = true; return r; };
    if (home_in.empty() || !home.is_absolute()) return refuse("no home folder given");
    if (home == home.root_path())               return refuse("home is the filesystem root");
    if (!fs::is_directory(home, ec))            return refuse("home is not a folder: " + r.home);
    if (!fs::is_directory(root, ec))            return refuse("not a folder: " + r.root);

    const fs::path relp = root.lexically_relative(home);
    const std::string rel = relp.string() == "." ? std::string{} : relp.string();
    if (relp.empty() || rel.rfind("..", 0) == 0)
        return refuse("outside the home folder: " + r.root);   // §5: nothing outside $HOME

    struct stat st{};
    if (::stat(r.root.c_str(), &st) != 0) return refuse("cannot read " + r.root);

    if (threads == 0) threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    Walker w(r.home, st.st_dev, p, mine != nullptr);
    Task first = root_task(r.home, rel);
    Local root_local;
    root_local.charge(first.kind, first.item, static_cast<std::uint64_t>(st.st_blocks) * 512u, false);
    if (first.pkg == rel) root_local.charge_pkg(first.pkg, static_cast<std::uint64_t>(st.st_blocks) * 512u, false);
    w.push(std::move(first));

    std::vector<Local> locals(threads);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back([&w, &locals, i] { w.run(locals[i]); });
    for (auto& th : pool) th.join();
    locals.push_back(std::move(root_local));

    std::map<std::string, Item> items, pkgs;
    std::map<std::string, Newest> pkg_new;
    for (auto& l : locals) {
        for (auto& [key, nw] : l.pkg_new) {
            auto& d = pkg_new[key];
            d.change = std::max(d.change, nw.change);
            d.read   = std::max(d.read, nw.read);
        }
        if (mine) for (auto& sn : l.mine) mine->push_back(std::move(sn));
        r.unwalked.insert(r.unwalked.end(), l.unwalked.begin(), l.unwalked.end());
        for (auto& [key, it] : l.pkgs) {
            auto [dst, fresh] = pkgs.try_emplace(key, Item{key, Kind::Other, 0, 0});
            dst->second.bytes += it.bytes;
            dst->second.files += it.files;
        }
        for (int k = 0; k < kKindCount; ++k) {
            r.by_kind[k].bytes += l.by_kind[k].bytes;
            r.by_kind[k].files += l.by_kind[k].files;
        }
        for (auto& [key, it] : l.items) {
            auto [dst, fresh] = items.try_emplace(key, Item{key, it.kind, 0, 0});
            dst->second.bytes += it.bytes;
            dst->second.files += it.files;
        }
        r.skipped.insert(r.skipped.end(), l.skipped.begin(), l.skipped.end());
        r.errors += l.errors;
        r.dirs   += l.dirs;
    }
    for (auto& [key, it] : items) r.items.push_back(std::move(it));
    std::sort(r.items.begin(), r.items.end(), [](const Item& a, const Item& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.rel < b.rel;
    });
    for (auto& [key, it] : pkgs) {
        if (auto n = pkg_new.find(key); n != pkg_new.end()) {
            it.newest_change = n->second.change;
            it.newest_read   = n->second.read;
        }
        r.packages.push_back(std::move(it));
    }
    if (mine) {
        std::map<std::string, const Item*> by;
        for (const auto& it : r.packages) by[it.rel] = &it;
        for (auto& sn : *mine) {
            if (!sn.package) continue;
            if (auto b = by.find(sn.rel); b != by.end()) {
                sn.bytes   = b->second->bytes;
                sn.changed = b->second->newest_change;
                sn.read    = b->second->newest_read ? b->second->newest_read : b->second->newest_change;
            }
        }
        std::sort(mine->begin(), mine->end(), [](const Seen& a, const Seen& b) { return a.rel < b.rel; });
    }
    std::sort(r.unwalked.begin(), r.unwalked.end());
    std::sort(r.packages.begin(), r.packages.end(), [](const Item& a, const Item& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.rel < b.rel;
    });
    std::sort(r.skipped.begin(), r.skipped.end());
    r.cancelled = p.cancel.load();
    p.done = true;
    return r;
}

}  // namespace rouster::core::scan
