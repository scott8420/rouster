#include "core/Sweep.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

// Sweep.cpp -- FIND and PLAN. Read-only: opendir/readdir/lstat and the scanner.
// No call in this file changes the disk; the source gate checks that.

namespace rouster::core::sweep {

const char* group_name(Group g) {
    switch (g) {
        case Group::Thumbnails: return "Thumbnails";
        case Group::Caches:     return "Caches";
        case Group::AppCaches:  return "App caches (Flatpak)";
        case Group::Logs:       return "Old logs";
    }
    return "?";
}

const char* group_why(Group g) {
    switch (g) {
        case Group::Thumbnails: return "Previews of your pictures. The file manager redraws them as you browse.";
        case Group::Caches:     return "What apps keep to start faster. Each one rebuilds itself on next use.";
        case Group::AppCaches:  return "The same, for Flatpak apps -- each app's own cache folder.";
        case Group::Logs:       return "Log files nobody has written to in a month. Unchecked: you may want one.";
    }
    return "";
}

const std::vector<std::string>& slow_to_rebuild() {
    static const std::vector<std::string> v = {
        "huggingface", "torch", "ms-playwright", "JetBrains",
        "pip", "pypoetry", "go-build", "bazel", "ccache", "sccache",
    };
    return v;
}

bool is_log_name(const std::string& n) {
    // name.log | name.log.<anything> (rotations, .gz, .old)
    if (n.size() > 4 && n.ends_with(".log")) return true;
    const auto p = n.find(".log.");
    return p != std::string::npos && p > 0 && p + 5 < n.size();
}

namespace {

bool starts_dir(const std::string& rel, const std::string& base) {
    return rel == base || (rel.size() > base.size() && rel.compare(0, base.size(), base) == 0 &&
                           rel[base.size()] == '/');
}

// The never-touch list and any fixes/ component -- the same rule the scanner
// applies, restated here for the log walk (the door applies it a third time).
bool protected_rel(const std::string& rel) {
    for (const auto& n : scan::never_touch())
        if (starts_dir(rel, n)) return true;
    std::size_t s = 0;
    while (s <= rel.size()) {
        const auto e = rel.find('/', s);
        const std::string part = rel.substr(s, e == std::string::npos ? std::string::npos : e - s);
        if (part == "fixes") return true;
        if (e == std::string::npos) break;
        s = e + 1;
    }
    return false;
}

std::uint64_t usage(const struct stat& st) { return static_cast<std::uint64_t>(st.st_blocks) * 512u; }

// lstat-only walk: one filesystem, no symlink followed, depth-capped.
template <class F>
void walk(const std::string& home, const std::string& rel, dev_t dev, int depth,
          scan::Progress& prog, std::uint64_t& errors, F&& on_file) {
    if (depth > 16 || prog.cancel.load(std::memory_order_relaxed)) return;
    const std::string abs = home + "/" + rel;
    DIR* d = ::opendir(abs.c_str());
    if (!d) { ++errors; return; }
    while (dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        const std::string crel = rel + "/" + e->d_name;
        struct stat st{};
        if (::lstat((home + "/" + crel).c_str(), &st) != 0) { ++errors; continue; }
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev != dev) continue;   // another filesystem: not entered
            prog.dirs.fetch_add(1, std::memory_order_relaxed);
            walk(home, crel, dev, depth + 1, prog, errors, on_file);
        } else if (S_ISREG(st.st_mode)) {
            prog.files.fetch_add(1, std::memory_order_relaxed);
            on_file(crel, e->d_name, st);
        }
        // symlinks, sockets, fifos: never followed, never offered
    }
    ::closedir(d);
}

}  // namespace

Found find(const std::string& home, scan::Progress& prog, std::int64_t now) {
    Found f;
    struct stat hs{};
    if (::lstat(home.c_str(), &hs) != 0 || !S_ISDIR(hs.st_mode)) {
        f.refused = "home is not a folder: " + home;
        prog.done = true;
        return f;
    }

    // 1. Caches, thumbnails, Flatpak caches -- straight from the classifier.
    const auto take = [&](const scan::Report& r) {
        f.errors += r.errors;
        if (r.cancelled) f.cancelled = true;
        for (const auto& it : r.items) {
            if (it.kind != scan::Kind::Regenerable) continue;
            Candidate c{it.rel, Group::Caches, it.bytes, it.files, true, ""};
            if (starts_dir(it.rel, ".cache")) {
                if (it.rel == ".cache") continue;                 // loose files in ~/.cache: the folder itself stays
                if (it.rel == ".cache/thumbnails") c.group = Group::Thumbnails;
                const std::string leaf = it.rel.substr(7);
                if (std::find(slow_to_rebuild().begin(), slow_to_rebuild().end(), leaf) != slow_to_rebuild().end()) {
                    c.checked = false;
                    c.note = "slow to get back -- a big download or a long reindex";
                }
            } else if (starts_dir(it.rel, ".var/app") && it.rel.ends_with("/cache")) {
                c.group = Group::AppCaches;
            } else {
                continue;                                         // build/ dirs belong to Dev projects (s007)
            }
            f.items.push_back(std::move(c));
        }
    };
    for (const char* sub : {".cache", ".var/app"}) {
        struct stat st{};
        const std::string root = home + "/" + sub;
        if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;   // absent, or a symlink: skipped
        prog.done = false;
        take(scan::scan(home, root, prog));
        if (prog.cancel.load()) break;
    }

    // 2. Old logs, with lstat alone.
    const std::int64_t cutoff = now - static_cast<std::int64_t>(kOldLogDays) * 86400;
    for (const char* sub : {".local/state", ".local/share"}) {
        if (prog.cancel.load()) break;
        struct stat st{};
        if (::lstat((home + "/" + sub).c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != hs.st_dev) continue;
        walk(home, sub, hs.st_dev, 0, prog, f.errors,
             [&](const std::string& rel, const char* name, const struct stat& fs) {
                 if (!is_log_name(name) || fs.st_mtim.tv_sec > cutoff) return;
                 if (protected_rel(rel) || starts_dir(rel, ".local/share/Trash") ||
                     starts_dir(rel, ".local/share/rouster"))
                     return;
                 f.items.push_back({rel, Group::Logs, usage(fs), 1, false, ""});
             });
    }

    // 3. The Trash, measured only.
    {
        const std::string t = ".local/share/Trash/files";
        struct stat st{};
        if (::lstat((home + "/" + t).c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == hs.st_dev && !prog.cancel.load())
            walk(home, t, hs.st_dev, 0, prog, f.errors,
                 [&](const std::string&, const char*, const struct stat& fs) {
                     f.trash_bytes += usage(fs);
                     ++f.trash_files;
                 });
    }

    if (prog.cancel.load()) f.cancelled = true;
    std::stable_sort(f.items.begin(), f.items.end(), [](const Candidate& a, const Candidate& b) {
        if (a.group != b.group) return static_cast<int>(a.group) < static_cast<int>(b.group);
        return a.bytes > b.bytes;
    });
    prog.done = true;
    return f;
}

door::Plan plan(const std::string& home, const std::vector<Candidate>& items) {
    door::Plan p;
    std::uint64_t bytes = 0;
    for (const auto& c : items) {
        if (!c.checked) continue;
        door::Action a;
        a.verb   = door::Verb::Trash;
        a.from   = home + "/" + c.rel;
        a.reason = std::string("Sweep: ") + group_name(c.group);
        a.bytes  = c.bytes;
        bytes += c.bytes;
        p.actions.push_back(std::move(a));
    }
    p.label = "Sweep: " + std::to_string(p.actions.size()) + (p.actions.size() == 1 ? " item, " : " items, ") +
              scan::human_bytes(bytes);
    p.previewed = false;
    return p;
}

}  // namespace rouster::core::sweep
