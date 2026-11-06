#include "core/Scan.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <thread>
#include <utility>

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
    if (parent == Kind::Protected) return {Kind::Protected, false, false};
    if (parent == Kind::Trash)     return {Kind::Trash, false, false};

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
};

struct Local {               // one worker's tallies, merged at the end
    std::array<Bucket, kKindCount>   by_kind{};
    std::map<std::string, Item>      items;
    std::vector<std::string>         skipped;
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
};

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
    Task t{rel, dc.kind, parent.item, false};
    if (dc.rooted)          { t.item = rel; t.expand = dc.expand; }
    else if (parent.expand) { t.item = rel; }
    if (t.kind == Kind::Other) t.item.clear();
    return t;
}

class Walker {
public:
    Walker(std::string home, dev_t dev, Progress& p) : m_home(std::move(home)), m_dev(dev), m_p(p) {}

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

    void visit(const Task& t, Local& out) {
        const std::string abs = t.rel.empty() ? m_home : m_home + "/" + t.rel;
        const int dfd = ::open(abs.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dfd < 0) { ++out.errors; return; }
        DIR* d = ::fdopendir(dfd);
        if (!d) { ::close(dfd); ++out.errors; return; }
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
                m_p.bytes.fetch_add(bytes, std::memory_order_relaxed);
                push(std::move(c));
                continue;
            }
            if (st.st_nlink > 1 && !first_link(st.st_dev, st.st_ino)) continue;
            const Kind k = classify_file(rel, t.kind);
            out.charge(k, k == t.kind ? t.item : rel, bytes, true);
            m_p.files.fetch_add(1, std::memory_order_relaxed);
            m_p.bytes.fetch_add(bytes, std::memory_order_relaxed);
        }
        ::closedir(d);   // closes dfd
    }

    std::string m_home;
    dev_t       m_dev;
    Progress&   m_p;

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
    Task t{"", Kind::Other, "", false};
    if (rel.empty()) return t;
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
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    return t;
}

}  // namespace

Report scan(const std::string& home_in, const std::string& root_in,
            Progress& p, unsigned threads) {
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
    Walker w(r.home, st.st_dev, p);
    Task first = root_task(r.home, rel);
    Local root_local;
    root_local.charge(first.kind, first.item, static_cast<std::uint64_t>(st.st_blocks) * 512u, false);
    w.push(std::move(first));

    std::vector<Local> locals(threads);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back([&w, &locals, i] { w.run(locals[i]); });
    for (auto& th : pool) th.join();
    locals.push_back(std::move(root_local));

    std::map<std::string, Item> items;
    for (auto& l : locals) {
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
    std::sort(r.skipped.begin(), r.skipped.end());
    r.cancelled = p.cancel.load();
    p.done = true;
    return r;
}

}  // namespace rouster::core::scan
