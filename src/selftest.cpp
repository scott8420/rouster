// rouster_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * Scan     -- the read-only walker over a generated fixture home: kinds,
//                 sizes, thread-invariance, nothing written, refusals.
//   * Dev      -- find/verify/lint are read-only (even .git is untouched); every
//                 repo state told apart; the light clean through the door and back.
//   * Sweep    -- find is read-only; the plan is only what is checked; the
//                 plan through the door and back leaves the tree exact.
//
//   cmake --build build --target rouster_selftest && ./build/rouster_selftest

#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/Scan.hpp"
#include "core/Preflight.hpp"
#include "core/Door.hpp"
#include "core/Sweep.hpp"
#include "core/Dev.hpp"
#include "core/Known.hpp"
#include "core/Dups.hpp"
#include "core/Archive.hpp"
#include "core/Organize.hpp"
#include "core/Rules.hpp"
#include "core/Settings.hpp"
#include "core/Profile.hpp"
#include "core/Helper.hpp"
#include "core/Help.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <random>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace core = rouster::core;

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) { ++g_pass; std::cout << "  PASS  " << name; }
    else    { ++g_fail; std::cout << "  FAIL  " << name; }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

// ── The fixture home (s002) ──────────────────────────────────────────────────
// A fake $HOME with every kind in it. Every scan test runs here -- never on the
// real home (RULES: fixtures, never the real home). Also reachable as
// `rouster_selftest --make-fixture DIR` for an Xvfb drive of the real app.
namespace fs = std::filesystem;

bool put(const fs::path& home, const std::string& rel, std::size_t bytes) {
    const fs::path p = home / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary);
    std::string chunk(4096, 'r');
    for (std::size_t left = bytes; left > 0;) {
        const std::size_t n = std::min(left, chunk.size());
        o.write(chunk.data(), static_cast<std::streamsize>(n));
        left -= n;
    }
    return static_cast<bool>(o);
}

// Exact bytes (Duplicates needs content that differs where we say it does).
bool put_bytes(const fs::path& home, const std::string& rel, const std::string& data) {
    const fs::path p = home / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary);
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}
std::string noise(std::size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(rng() & 0xff);
    return s;
}
// Where a fixture can share blocks (s008): the temp dir if it is btrfs, else
// /var/tmp if that is (Fedora: /tmp is tmpfs, /var/tmp is on the btrfs root),
// else the temp dir (Dedupe is then checked to fail cleanly).
fs::path share_base() {
    const fs::path t = fs::temp_directory_path();
    if (rouster::core::dups::can_share(t.string())) return t;
    if (rouster::core::dups::can_share("/var/tmp")) return "/var/tmp";
    return t;
}

// Expected file counts per kind are written next to the tree they describe.
bool make_fixture(const fs::path& home) {
    std::error_code ec;
    if (fs::exists(home, ec) && !fs::is_empty(home, ec)) return false;   // never over a real folder
    fs::create_directories(home, ec);
    bool ok = true;
    // Regenerable (6)
    ok &= put(home, ".cache/mozilla/firefox/cache2/a.bin", 100000);
    ok &= put(home, ".cache/thumbnails/large/t.png", 20000);
    ok &= put(home, ".cache/top.txt", 10);
    ok &= put(home, ".var/app/org.example.Y/cache/c.bin", 30000);
    ok &= put(home, "Develop/proj/rouster/build/CMakeCache.txt", 5000);
    ok &= put(home, "Develop/proj/rouster/build/obj/main.o", 70000);
    // Settings (2)
    ok &= put(home, ".config/app1/settings.ini", 300);
    ok &= put(home, ".var/app/org.example.Y/config/c.ini", 200);
    // State (2)
    ok &= put(home, ".local/state/wireplumber/state", 400);
    ok &= put(home, ".bash_history", 9000);
    // App data (2)
    ok &= put(home, ".local/share/app2/db.sqlite", 50000);
    ok &= put(home, ".var/app/org.example.Y/data/d.bin", 40000);
    // Trash (2)
    ok &= put(home, ".local/share/Trash/files/old.txt", 60000);
    ok &= put(home, ".local/share/Trash/info/old.txt.trashinfo", 100);
    // Protected (4)
    ok &= put(home, ".config/dconf/user", 3000);
    ok &= put(home, ".local/share/keyrings/login.keyring", 2000);
    ok &= put(home, ".ssh/id_ed25519", 400);
    ok &= put(home, "Develop/proj/fixes/patch.diff", 800);
    // Your files (6: four files, a hard link that is not counted, two symlinks)
    ok &= put(home, "Develop/proj/rouster/src/main.cpp", 1200);
    ok &= put(home, "Documents/build/notes.txt", 700);     // "build" with no marker: still yours
    ok &= put(home, "Documents/report.pdf", 25000);
    ok &= put(home, "Pictures/a.jpg", 80000);
    fs::create_hard_link(home / "Pictures/a.jpg", home / "Pictures/a_link.jpg", ec); ok &= !ec;
    fs::create_directory_symlink("..", home / "Documents/loop", ec);   ok &= !ec;   // a cycle, if followed
    fs::create_directory_symlink("/etc", home / "Documents/etc", ec);  ok &= !ec;   // outside, if followed
    return ok;
}

// Files only (path, mode, size, mtime) -- what a user sees. Directory mtimes
// move whenever something is renamed in or out, so "put back exactly" is
// judged on the files and the shape of the tree.
// An in-memory dconf for the door's Settings verb (s012d): dump / load / reset,
// key paths -> values. `fail_load` makes the next load fail.
struct FakeDconf {
    std::map<std::string, std::string> keys;
    bool fail_load = false;
    std::vector<std::string> calls;
    std::string dump() const {
        std::map<std::string, std::vector<std::string>> dirs;
        for (const auto& [k, v] : keys) {
            const auto cut = k.rfind('/');
            const std::string dir = k.substr(1, cut == 0 ? 0 : cut - 1);
            dirs[dir.empty() ? "/" : dir].push_back(k.substr(cut + 1) + "=" + v);
        }
        std::string out;
        for (const auto& [d, ls] : dirs) {
            out += "[" + d + "]\n";
            for (const auto& l : ls) out += l + "\n";
            out += "\n";
        }
        return out;
    }
    core::vcs::Run operator()(const std::vector<std::string>& a, const std::string& in) {
        core::vcs::Run r;
        calls.push_back(a.size() > 1 ? a[1] : "");
        if (a.size() >= 3 && a[1] == "dump") { r.code = 0; r.out = dump(); }
        else if (a.size() >= 3 && a[1] == "load") {
            if (fail_load) { r.code = 1; r.err = "error: fake failure\n"; return r; }
            const auto ks = core::door::dconf_keys(in);
            std::size_t i = 0;
            std::istringstream s(in);
            for (std::string l; std::getline(s, l);) {
                if (l.empty() || l[0] == '#' || l[0] == '[') continue;
                const auto eq = l.find('=');
                if (eq != std::string::npos && i < ks.size()) keys[ks[i++]] = l.substr(eq + 1);
            }
            r.code = 0;
        } else if (a.size() >= 3 && a[1] == "reset") { keys.erase(a[2]); r.code = 0; }
        else r.code = 127;
        return r;
    }
};

std::string files_snapshot(const fs::path& root) {
    std::vector<std::string> rows;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        struct stat st{};
        ::lstat(it->path().c_str(), &st);
        std::string row = it->path().string() + "|" + std::to_string(st.st_mode);
        if (!S_ISDIR(st.st_mode))
            row += "|" + std::to_string(st.st_size) + "|" + std::to_string(st.st_mtim.tv_sec) + "." +
                   std::to_string(st.st_mtim.tv_nsec) + "|" + std::to_string(st.st_ino);
        // The manifest and the Trash's empty folders are the door's own trail.
        const std::string p = it->path().string();
        if (p.find("/.local/share/rouster") != std::string::npos) continue;
        if (S_ISDIR(st.st_mode) && (p.ends_with("/Trash/files") || p.ends_with("/Trash/info"))) continue;
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (auto& r : rows) out += r + "\n";
    return out;
}

// What's on disk, independently: du's rule (st_blocks, hard links once, no
// symlink followed), single-threaded via std::filesystem.
struct Du { std::uint64_t bytes = 0, files = 0, dirs = 0; };
Du du(const fs::path& root) {
    Du d;
    std::set<std::pair<dev_t, ino_t>> seen;
    const auto add = [&](const fs::path& p, bool dir) {
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0) return;
        if (!dir && st.st_nlink > 1 && !seen.emplace(st.st_dev, st.st_ino).second) return;
        d.bytes += static_cast<std::uint64_t>(st.st_blocks) * 512u;
        (dir ? d.dirs : d.files)++;
    };
    add(root, true);
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it)
        add(it->path(), it->is_directory() && !it->is_symlink());
    return d;
}

// Everything a write could change: path, type, size, mtime, mode.
std::string tree_snapshot(const fs::path& root) {
    std::vector<std::string> rows;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        struct stat st{};
        ::lstat(it->path().c_str(), &st);
        rows.push_back(it->path().string() + "|" + std::to_string(st.st_mode) + "|" +
                       std::to_string(st.st_size) + "|" + std::to_string(st.st_mtim.tv_sec) + "." +
                       std::to_string(st.st_mtim.tv_nsec));
    }
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (auto& r : rows) out += r + "\n";
    return out;
}

// ── The dev fixture (s007) ──────────────────────────────────────────────────
// Real git repos under <home>/Develop, their "GitHub" a folder of bare repos
// OUTSIDE home (remotes). Every repo state Dev projects must tell apart:
//   rouster_ws/  Scott's workspace, all pushed            -> Backed, 2 build dirs
//   baker/       a drifted workspace, dirty every way     -> NotClean, lint drift
//   slate/       a plain repo with no remote              -> NoRemote, build/ left alone
//   tools/scratch/  remote path that doesn't exist        -> Unreachable
//   behind/      the remote moved on, fetched here        -> Backed (nothing local is missing)
//   stale/       the remote moved on, NOT fetched         -> NotClean ("fetch, then look again")
//   misc/notes/  just a folder                            -> not a project
// Git runs isolated from the machine's config (GIT_CONFIG_GLOBAL=/dev/null).
void git_isolate() {
    ::setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
    ::setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    ::setenv("GIT_AUTHOR_NAME", "Fixture", 1);
    ::setenv("GIT_AUTHOR_EMAIL", "fixture@example.invalid", 1);
    ::setenv("GIT_COMMITTER_NAME", "Fixture", 1);
    ::setenv("GIT_COMMITTER_EMAIL", "fixture@example.invalid", 1);
}
bool sh(const std::string& dir, const std::string& cmd) {
    const std::string full = "cd '" + dir + "' && { " + cmd + " ; } >/dev/null 2>&1";
    return std::system(full.c_str()) == 0;
}
bool make_dev_fixture(const fs::path& home, const fs::path& remotes) {
    git_isolate();
    std::error_code ec;
    fs::create_directories(remotes, ec);
    const std::string R = remotes.string();
    const std::string init = "git -c init.defaultBranch=main init -q";
    bool ok = true;
    const auto bare = [&](const std::string& name) { return sh(R, init + " --bare " + name + ".git"); };

    // rouster_ws: the template, clean and pushed, with a tag.
    const fs::path ws = home / "Develop/rouster_ws";
    ok &= put(ws, "docs/RULES.md", 100);
    ok &= put(ws, "scripts/rouster_backup.sh", 100);
    fs::create_directories(ws / "backups/Old", ec);
    ok &= put(ws, "fixes/patch.diff", 200);
    ok &= put(ws, "rouster/CMakeLists.txt", 0);
    { std::ofstream(ws / "rouster/CMakeLists.txt") << "project(rouster)\nset(APP_ID io.github.scott8420.Rouster)\n"; }
    { std::ofstream(ws / "rouster/.gitignore") << "build/\n*.local\n"; }
    ok &= put(ws, "rouster/build.sh", 50);
    ok &= put(ws, "rouster/include/a.hpp", 50);
    ok &= put(ws, "rouster/src/main.cpp", 300);
    ok &= put(ws, "rouster/resources/r.css", 50);
    ok &= put(ws, "rouster/data/io.github.scott8420.Rouster.desktop.in", 50);
    ok &= put(ws, "rouster/libs/foo/CMakeLists.txt", 20);
    ok &= put(ws, "rouster/build/obj/main.o", 90000);
    ok &= put(ws, "rouster/libs/foo/build/foo.o", 30000);
    ok &= put(ws, "rouster/.secrets.local", 20);                    // ignored, not build output (s007b)
    ok &= put(ws, "backups/s001_a.zip", 3000);                       // older: retire thins it
    ok &= put(ws, "backups/s002_b.zip", 3000);                       // newest: retire keeps it (Q1)
    ok &= put(ws, "backups/Old/s000_z.zip", 3000);
    {
        struct timespec ts[2] = {{1700000000, 0}, {1700000000, 0}};
        ::utimensat(AT_FDCWD, (ws / "backups/s001_a.zip").c_str(), ts, 0);
    }
    ok &= bare("rouster");
    ok &= sh((ws / "rouster").string(), init + " && git add -A && git commit -qm one && git tag -a v1 -m v1 && "
             "git remote add origin '" + R + "/rouster.git' && git push -q -u origin main && git push -q origin v1");

    // baker: drift, and not clean in every way the checks know.
    const fs::path bk = home / "Develop/baker";
    ok &= put(bk, "docs/RULES.md", 100);
    ok &= put(bk, "scripts/backup.sh", 100);
    { fs::create_directories(bk / "baker", ec); std::ofstream(bk / "baker/CMakeLists.txt") << "set(ID io.github.example.Baker)\n"; }
    ok &= put(bk, "baker/build.sh", 50);
    ok &= put(bk, "baker/src/main.cpp", 300);                       // no include/: required, missing
    ok &= put(bk, "baker/resources/io.github.example.Baker.desktop.in", 50);
    ok &= put(bk, "baker/notes/todo.txt", 50);                      // unknown folder
    ok &= put(bk, "baker/build/README", 10);                        // a TRACKED build/: not build output
    ok &= put(bk, "stray.txt", 10);                                 // unknown at the workspace top
    ok &= bare("baker");
    ok &= sh((bk / "baker").string(), init + " && git add -A && git commit -qm one && "
             "git remote add origin '" + R + "/baker.git' && git push -q -u origin main && "
             "echo two >> src/main.cpp && git commit -qam two && "              // unpushed commit
             "git branch side && "                                             // local-only branch
             "git tag v0 && "                                                  // unpushed tag
             "echo stash >> build.sh && git stash -q && "                      // a stash
             "echo dirty >> build.sh");                                        // uncommitted

    // slate: no remote at all.
    const fs::path sl = home / "Develop/slate";
    ok &= put(sl, "main.cpp", 100);
    { fs::create_directories(sl, ec); std::ofstream(sl / ".gitignore") << "build/\n"; }
    ok &= put(sl, "build/slate.o", 40000);
    ok &= sh(sl.string(), init + " && git add -A && git commit -qm one");

    // tools/scratch: the remote isn't there.
    const fs::path sc = home / "Develop/tools/scratch";
    ok &= put(sc, "x.txt", 10);
    ok &= sh(sc.string(), init + " && git add -A && git commit -qm one && git remote add origin '" + R + "/gone.git'");

    // behind / stale: someone else pushed; one fetched, one didn't.
    ok &= bare("shared");
    const fs::path other = remotes / "other_clone";
    ok &= sh(R, "git clone -q shared.git other_clone");
    ok &= put(other, "a.txt", 10);
    ok &= sh(other.string(), "git add -A && git commit -qm a && git push -q origin HEAD:main");
    for (const char* n : {"behind", "stale"})
        ok &= sh((home / "Develop").string(), std::string("git clone -q '") + R + "/shared.git' " + n);
    ok &= put(other, "b.txt", 10);
    ok &= sh(other.string(), "git add -A && git commit -qm b && git push -q origin HEAD:main");
    ok &= sh((home / "Develop/behind").string(), "git fetch -q");

    ok &= put(home, "Develop/misc/notes/n.txt", 10);
    return ok;
}
}  // namespace

// The duplicates a drive should show (s008): a double-dumped backup folder,
// a download saved twice, a "copy" of a photo, one file of the backup elsewhere.
bool make_dups_fixture(const fs::path& home) {
    bool ok = true;
    for (const char* top : {"Backups/2024", "Backups/2024 (2)"}) {
        ok &= put_bytes(home, std::string(top) + "/notes.odt", noise(60000, 21));
        ok &= put_bytes(home, std::string(top) + "/photos/beach.jpg", noise(240000, 22));
        ok &= put_bytes(home, std::string(top) + "/photos/dune.jpg", noise(180000, 23));
    }
    ok &= put_bytes(home, "Documents/invoice.pdf", noise(90000, 24));
    ok &= put_bytes(home, "Downloads/invoice (1).pdf", noise(90000, 24));
    ok &= put_bytes(home, "Pictures/cat.png", noise(120000, 25));
    ok &= put_bytes(home, "Pictures/cat copy.png", noise(120000, 25));
    ok &= put_bytes(home, "Desktop/beach.jpg", noise(240000, 22));
    return ok;
}

// Old files for an Archive drive (s009): a 2019 tax folder (one item), a
// 2018 trip in Pictures, a 2021 letter, an old installer in Downloads.
bool make_archive_fixture(const fs::path& home) {
    const std::vector<std::pair<std::string, std::int64_t>> f = {
        {"Documents/Taxes 2019/return.pdf", 1556000000}, {"Documents/Taxes 2019/receipts/r1.pdf", 1556000000},
        {"Documents/Taxes 2019/receipts/r2.pdf", 1556100000}, {"Pictures/Trip 2018/day1.jpg", 1530000000},
        {"Pictures/Trip 2018/day2.jpg", 1530100000}, {"Documents/old-letter.odt", 1620000000},
        {"Downloads/setup-2.4.iso", 1650000000}, {"Documents/active.odt", 0}};
    bool ok = true;
    for (const auto& [rel, t] : f) {
        ok &= put(home, rel, 30000);
        if (!t) continue;
        struct timespec ts[2] = {{t, 0}, {t, 0}};
        ::utimensat(AT_FDCWD, (home / rel).c_str(), ts, AT_SYMLINK_NOFOLLOW);
    }
    return ok;
}

// Loose files to file into a project (s010): a logo, a font, a photo, a note.
bool make_organize_fixture(const fs::path& home) {
    bool ok = true;
    for (const char* r : {"Downloads/Acme logo FINAL.ai", "Downloads/Brandon Grotesque.otf", "Downloads/shoot 01.JPG",
                          "Downloads/call notes.txt"})
        ok &= put(home, r, 12000);
    return ok;
}

// A set-up home (s012): dotfiles, an ssh config, an extension, autostart, a
// user unit enabled by a link, a font -- what a profile backup gathers.
bool make_profile_fixture(const fs::path& home) {
    bool ok = true;
    ok &= put_bytes(home, ".bashrc", "# fixture bashrc\nalias ll='ls -l'\n");
    ok &= put_bytes(home, ".gitconfig", "[user]\n\tname = Fixture\n");
    ok &= put_bytes(home, ".ssh/config", "Host gh\n  HostName github.com\n");
    ok &= put_bytes(home, ".ssh/id_ed25519", "NOT A REAL KEY");
    ok &= put_bytes(home, ".local/share/gnome-shell/extensions/dash@fixture/metadata.json",
                    "{\"uuid\":\"dash@fixture\",\"name\":\"Dash Fixture\",\"version\":7}");
    ok &= put_bytes(home, ".config/autostart/notes.desktop", "[Desktop Entry]\nName=Notes\nExec=true\n");
    ok &= put_bytes(home, ".config/systemd/user/sync.service", "[Service]\nExecStart=/bin/true\n");
    std::error_code ec;
    fs::create_directories(home / ".config/systemd/user/default.target.wants", ec);
    fs::create_symlink("../sync.service", home / ".config/systemd/user/default.target.wants/sync.service", ec);
    ok &= !ec;
    ok &= put(home, ".local/share/fonts/Fixture Sans.otf", 20000);
    return ok;
}

int main(int argc, char** argv) {
    // `--make-fixture DIR`: build the fixture home for an Xvfb drive, then stop.
    if (argc == 3 && std::string(argv[1]) == "--make-fixture") {
        const bool ok = make_fixture(argv[2]) &&
                        make_dev_fixture(argv[2], std::string(argv[2]) + "_remotes") &&   // s007
                        make_dups_fixture(argv[2]) &&                                       // s008
                        make_archive_fixture(argv[2]) &&                                    // s009
                        make_organize_fixture(argv[2]) &&                                   // s010
                        make_profile_fixture(argv[2]);                                      // s012
        // s012d: a few settings in the FIXTURE's own dconf (never the desktop's),
        // when dconf + dbus-run-session are here; skipped quietly when not.
        if (ok) {
            const auto r = core::door::fixture_dconf(argv[2])(
                {"dconf", "load", "/"},
                "[org/gnome/desktop/interface]\ncolor-scheme='prefer-dark'\nclock-format='24h'\n\n"
                "[org/gnome/desktop/wm/preferences]\nbutton-layout='appmenu:minimize,close'\n");
            std::cout << (r.code == 0 ? "fixture dconf: 3 settings in " : "fixture dconf: not seeded (no dconf / dbus-run-session) -- ")
                      << argv[2] << "/.config/dconf\n";
        }
        std::cout << (ok ? "fixture: " : "fixture FAILED (exists and not empty?): ") << argv[2] << "\n";
        return ok ? 0 : 1;
    }
    std::cout << "rouster_selftest -- undo, recents, scan, preflight, door, source gate, shortcuts\n";

    // ── Undo -- the one door, two pictures, gestures, merging (s046) ──────────
    {
        std::cout << "\n-- undo --\n";
        using core::Stones;
        using core::Stone;
        using Clock = core::StoneJournal::Clock;
        using SnapMap = std::vector<std::pair<std::string, Stone>>;
        const auto snap = [](const Stones& s) {
            SnapMap out;
            for (const auto& id : s.ids()) out.emplace_back(id, *s.get(id));
            return out;
        };
        const auto names = [](const Stones& s) {
            std::string out;
            for (const auto& id : s.ids()) out += s.get(id)->name + ",";
            return out;
        };
        // A fixture: three stones, LOADED (not a user choice -- not undoable).
        struct Fix {
            Stones s;
            core::StoneJournal j{s};
            std::vector<std::string> sel;
            Fix() {
                s.set_door([this] { return j.open(); });
                j.set_selection_source([this] { return sel; });
                Stones::Loading load(s);
                for (const char* n : {"Ruby", "Jade", "Onyx"}) {
                    const auto id = s.fresh_id();
                    s.put(id, Stone{n, 3, s.pos_after({})});
                }
            }
        };

        // THE GATE. Every verb the page offers, from a fresh fixture: nothing
        // written with the door shut; at least one step; undoing them all puts
        // the model back exactly; redoing them all reproduces the result.
        for (const auto& v : core::stones::verbs()) {
            Fix f;
            const auto first = f.s.ids().front();
            const auto before = snap(f.s);
            v.run(f.j, f.s, first);
            const auto after = snap(f.s);
            const auto steps = f.j.size();
            check("gate/" + v.name + ": through the one door", f.s.strays() == 0,
                  std::to_string(f.s.strays()) + " stray write(s)");
            check("gate/" + v.name + ": is a step", steps > 0 && !(after == before));
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("gate/" + v.name + ": undo puts it back exactly", snap(f.s) == before);
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("gate/" + v.name + ": redo reproduces it", snap(f.s) == after);
        }
        {   // ...and the gate bites: a write around the door is counted.
            Fix f;
            const auto id = f.s.ids().front();
            f.s.put(id, Stone{"sneaky", 3, 0.5});
            check("gate: a write around the door is a stray", f.s.strays() == 1);
        }

        {   // Delete comes back whole, in its place, under its id, selected.
            Fix f;
            const auto ids = f.s.ids();
            f.sel = {ids[1]};
            core::stones::remove(f.j, f.s, ids[1]);
            f.sel = {ids[2]};      // the UI selects the neighbour...
            f.j.reselect(f.sel);   // ...and tells the step
            check("delete: gone", names(f.s) == "Ruby,Onyx,", names(f.s));
            check("delete: the label names it", f.j.undo_label() == "Delete “Jade”", f.j.undo_label());
            const auto r = f.j.undo();
            check("delete: undo -- back in its place", names(f.s) == "Ruby,Jade,Onyx,", names(f.s));
            check("delete: undo -- the same id", f.s.ids()[1] == ids[1]);
            check("delete: undo hands back its selection", r.selection == std::vector<std::string>{ids[1]});
            const auto r2 = f.j.redo();
            check("delete: redo -- gone again, the neighbour selected",
                  names(f.s) == "Ruby,Onyx," && r2.selection == std::vector<std::string>{ids[2]});
        }

        {   // Created records: undo removes, redo brings back the same id.
            Fix f;
            const auto first = f.s.ids()[0];
            const auto made = core::stones::add(f.j, f.s, "Opal", first);
            check("add: just after the one given", names(f.s) == "Ruby,Opal,Jade,Onyx,", names(f.s));
            f.j.undo();
            check("add: undone", !f.s.get(made));
            f.j.redo();
            check("add: redone, same id, same place", f.s.get(made) && names(f.s) == "Ruby,Opal,Jade,Onyx,");
        }

        {   // A no-op is not a step.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::rename(f.j, f.s, id, "Ruby");
            core::stones::remove(f.j, f.s, "nope");
            core::stones::move_up(f.j, f.s, id);   // already first
            check("no-op: nothing kept", f.j.size() == 0 && !f.j.can_undo());
        }

        {   // Merging: arrows on the slider fold into one step inside the window.
            Fix f;
            const auto id = f.s.ids()[0];
            const auto t0 = Clock::now();
            core::stones::resize(f.j, f.s, id, 4);
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 5; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(400));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 6; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(800));
            check("merge: three presses, one step", f.j.size() == 1, std::to_string(f.j.size()));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 7; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::seconds(10));
            check("merge: after the window, a new step", f.j.size() == 2);
            f.j.undo();
            f.j.undo();
            check("merge: two undos, back to the start", f.s.get(id)->size == 3);
            // Another stone's key never merges into this one's step.
            Fix g;
            const auto a = g.s.ids()[0], b = g.s.ids()[1];
            core::stones::resize(g.j, g.s, a, 5);
            core::stones::resize(g.j, g.s, b, 5);
            check("merge: a different key is a different step", g.j.size() == 2);
            // Nor into a step that was undone.
            core::stones::resize(g.j, g.s, b, 6);
            g.j.undo();
            core::stones::resize(g.j, g.s, b, 7);
            check("merge: never into an undone step", g.j.size() == 2 && g.j.can_undo() && !g.j.can_redo());
            // Back to where it started inside the window: the step goes away.
            Fix h;
            const auto c = h.s.ids()[0];
            core::stones::resize(h.j, h.s, c, 4);
            core::stones::resize(h.j, h.s, c, 3);
            check("merge: there and back again is no step", h.j.size() == 0);
        }

        {   // A gesture is one step, however many writes the drag made.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::drag_begin(f.j, f.s, id);
            for (int n = 4; n <= 9; ++n) core::stones::drag_to(f.j, f.s, id, n);
            check("gesture: open while dragging", f.j.in_gesture() && f.j.size() == 0);
            core::stones::drag_end(f.j);
            check("gesture: a six-write drag is one step", f.j.size() == 1 && f.s.get(id)->size == 9);
            f.j.undo();
            check("gesture: one undo, back to the start", f.s.get(id)->size == 3);
            // Escape mid-drag: back, and nothing kept.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 7);
            f.j.cancel();
            check("gesture: cancel puts it back and keeps nothing",
                  f.s.get(id)->size == 3 && !f.j.in_gesture() && f.j.can_redo());
            // Ctrl+Z mid-drag: the drag becomes a step, then is undone.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 2);
            f.j.undo();
            check("gesture: undo mid-drag closes the drag, then undoes it",
                  !f.j.in_gesture() && f.s.get(id)->size == 3 && f.j.can_redo());
            check("gesture: no stray writes", f.s.strays() == 0);
        }

        {   // A new step forgets the redo branch; Move keeps both stones in one step.
            Fix f;
            const auto ids = f.s.ids();
            core::stones::move_down(f.j, f.s, ids[0]);
            check("move: one step for both stones", f.j.size() == 1 && names(f.s) == "Jade,Ruby,Onyx,", names(f.s));
            f.j.undo();
            check("move: undone", names(f.s) == "Ruby,Jade,Onyx,");
            core::stones::rename(f.j, f.s, ids[2], "Coal");
            check("redo branch: a new step forgets it", !f.j.can_redo() && f.j.size() == 1);
        }

        {   // The cap: the oldest step falls off; the newest still undoes.
            Fix f;
            const auto id = f.s.ids()[0];
            for (std::size_t i = 0; i < core::StoneJournal::kMax + 20; ++i)
                core::stones::rename(f.j, f.s, id, "n" + std::to_string(i));
            check("cap: kMax steps kept", f.j.size() == core::StoneJournal::kMax);
            f.j.undo();
            check("cap: the newest still undoes", f.s.get(id)->name == "n" + std::to_string(core::StoneJournal::kMax + 18));
        }

        {   // on_change: the UI hears every journal change (labels, greying).
            Fix f;
            int heard = 0;
            f.j.set_on_change([&] { ++heard; });
            core::stones::rename(f.j, f.s, f.s.ids()[0], "X");
            f.j.undo();
            f.j.redo();
            f.j.clear();
            check("on_change: step, undo, redo, clear -- four", heard == 4, std::to_string(heard));
        }
    }

    std::cout << "-----------------------------------------------\n";

    // -- Recents: list ops + JSON round-trip with prune -----------------------
    {
        std::vector<std::string> l;
        core::recents_add(l, "/a");
        core::recents_add(l, "/b");
        core::recents_add(l, "/c");
        check("recents: newest first", l.size() == 3 && l.front() == "/c");
        core::recents_add(l, "/a");
        check("recents: re-add moves to front (no dup)",
              l.size() == 3 && l.front() == "/a");
        core::recents_add(l, "/x/");
        check("recents: trailing slash normalised",
              std::find(l.begin(), l.end(), "/x") != l.end() &&
              std::find(l.begin(), l.end(), "/x/") == l.end());
        std::vector<std::string> cap;
        for (int i = 0; i < 20; ++i)
            core::recents_add(cap, "/p" + std::to_string(i), 8);
        check("recents: trim to max (8)", cap.size() == 8 && cap.front() == "/p19");

        const std::string base =
            (std::filesystem::temp_directory_path() / "rouster_selftest_recents").string();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        const std::string live = base + "/live";
        std::filesystem::create_directories(live, ec);
        const std::string file = base + "/recent.json";

        std::vector<std::string> to_save = {live, base + "/GONE"};
        check("recents: save ok", core::save_recents(file, to_save));
        auto loaded = core::load_recents(file);
        check("recents: load prunes the missing path",
              loaded.size() == 1 && loaded.front() == live);
        check("recents: missing file -> empty",
              core::load_recents(base + "/nope.json").empty());
    }

    // -- Scan: the read-only walker over the fixture home (s002) --------------
    {
        std::cout << "\n-- scan --\n";
        namespace scan = core::scan;
        using scan::Kind;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_home";
        std::error_code ec;
        fs::remove_all(home, ec);
        check("fixture: built under a temp dir", make_fixture(home), home.string());
        check("fixture: refuses to build over a non-empty folder", !make_fixture(home));

        const auto files = [](const scan::Report& r, Kind k) { return r.by_kind[static_cast<int>(k)].files; };
        const auto bytes = [](const scan::Report& r, Kind k) { return r.by_kind[static_cast<int>(k)].bytes; };
        const auto has_item = [](const scan::Report& r, const std::string& rel, Kind k) {
            for (const auto& i : r.items) if (i.rel == rel) return i.kind == k;
            return false;
        };

        const std::string before = tree_snapshot(home);
        scan::Progress p;
        const scan::Report r = scan::scan(home.string(), home.string(), p);
        check("scan: ran", r.refused.empty() && !r.cancelled && p.done, r.refused);
        check("scan: nothing on disk changed", tree_snapshot(home) == before);

        const std::pair<Kind, std::uint64_t> want[] = {
            {Kind::Regenerable, 6}, {Kind::Settings, 2}, {Kind::State, 2}, {Kind::AppData, 2},
            {Kind::Trash, 2}, {Kind::Protected, 4}, {Kind::Other, 6}};
        for (auto [k, n] : want)
            check(std::string("scan: ") + scan::kind_name(k) + " -- " + std::to_string(n) + " files",
                  files(r, k) == n, std::to_string(files(r, k)));

        const Du d = du(home);
        check("scan: total bytes match du (hard link once, no symlink followed)",
              r.total_bytes() == d.bytes, std::to_string(r.total_bytes()) + " vs " + std::to_string(d.bytes));
        check("scan: dirs match (the symlinked cycle is not entered)", r.dirs == d.dirs,
              std::to_string(r.dirs) + " vs " + std::to_string(d.dirs));
        check("scan: progress counters agree with the report",
              p.bytes.load() + 0 <= r.total_bytes() && p.files.load() == d.files, std::to_string(p.files.load()));
        check("scan: no errors, nothing skipped", r.errors == 0 && r.skipped.empty());

        check("scan: .cache children are items", has_item(r, ".cache/mozilla", Kind::Regenerable) &&
                                                 has_item(r, ".cache/thumbnails", Kind::Regenerable));
        check("scan: a marked build dir is one Regenerable item",
              has_item(r, "Develop/proj/rouster/build", Kind::Regenerable));
        check("scan: a build dir with no marker stays yours",
              !has_item(r, "Documents/build", Kind::Regenerable));
        check("scan: flatpak cache / config / data split",
              has_item(r, ".var/app/org.example.Y/cache", Kind::Regenerable) &&
              has_item(r, ".var/app/org.example.Y/config", Kind::Settings) &&
              has_item(r, ".var/app/org.example.Y/data", Kind::AppData));
        check("scan: shell history is State", has_item(r, ".bash_history", Kind::State));
        check("scan: fixes/ is Protected", has_item(r, "Develop/proj/fixes", Kind::Protected));
        check("scan: .config/dconf is Protected, not Settings", has_item(r, ".config/dconf", Kind::Protected));
        check("scan: Trash is its own kind", has_item(r, ".local/share/Trash", Kind::Trash));
        {
            bool sorted = true;
            for (std::size_t i = 1; i < r.items.size(); ++i) sorted &= r.items[i - 1].bytes >= r.items[i].bytes;
            check("scan: items biggest first", sorted);
        }

        // The same tree, one thread vs many: identical answer.
        scan::Progress p1, p8;
        const auto r1 = scan::scan(home.string(), home.string(), p1, 1);
        const auto r8 = scan::scan(home.string(), home.string(), p8, 8);
        bool same = r1.items.size() == r8.items.size();
        for (int k = 0; k < scan::kKindCount; ++k)
            same &= r1.by_kind[k].bytes == r8.by_kind[k].bytes && r1.by_kind[k].files == r8.by_kind[k].files;
        for (std::size_t i = 0; same && i < r1.items.size(); ++i)
            same &= r1.items[i].rel == r8.items[i].rel && r1.items[i].bytes == r8.items[i].bytes;
        check("scan: 1 thread and 8 threads agree", same);

        // A subfolder classifies as it would in the full scan.
        {
            scan::Progress q;
            const auto s = scan::scan(home.string(), (home / "Develop/proj/rouster").string(), q);
            check("scan/sub: a project folder -- build is Regenerable, src is yours",
                  s.refused.empty() && files(s, Kind::Regenerable) == 2 && files(s, Kind::Other) == 1);
            scan::Progress q2;
            const auto b = scan::scan(home.string(), (home / "Develop/proj/rouster/build/obj").string(), q2);
            check("scan/sub: inside a build dir -- still Regenerable",
                  files(b, Kind::Regenerable) == 1 && bytes(b, Kind::Other) == 0);
            scan::Progress q3;
            const auto c = scan::scan(home.string(), (home / ".cache").string(), q3);
            check("scan/sub: ~/.cache alone -- 3 Regenerable files", files(c, Kind::Regenerable) == 3);
            scan::Progress q4;
            const auto k = scan::scan(home.string(), (home / ".ssh").string(), q4);
            check("scan/sub: ~/.ssh alone -- Protected", files(k, Kind::Protected) == 1 && files(k, Kind::Other) == 0);
        }

        // Refusals: nothing outside home, no "/" as home.
        {
            scan::Progress q;
            check("scan/refuse: a root outside home",
                  !scan::scan(home.string(), fs::temp_directory_path().string(), q).refused.empty());
            scan::Progress q2;
            check("scan/refuse: a root that climbs out with ..",
                  !scan::scan(home.string(), (home / "Documents/../..").string(), q2).refused.empty());
            scan::Progress q3;
            check("scan/refuse: / as home", !scan::scan("/", "/", q3).refused.empty());
            scan::Progress q4;
            check("scan/refuse: a missing folder",
                  !scan::scan(home.string(), (home / "nope").string(), q4).refused.empty());
        }

        // Cancel before it starts: it stops, says so, and still finishes cleanly.
        {
            scan::Progress q;
            q.cancel = true;
            const auto c = scan::scan(home.string(), home.string(), q);
            check("scan/cancel: reported, and no file counted", c.cancelled && c.dirs == 0 && q.done);
        }

        // The never-touch list, rule by rule: each entry refuses every other kind.
        {
            bool all = true;
            for (const auto& rel : scan::never_touch())
                for (Kind parent : {Kind::Other, Kind::Regenerable, Kind::Settings, Kind::AppData})
                    all &= scan::classify_dir(rel, parent, false).kind == Kind::Protected;
            check("never-touch: every entry is Protected under any parent", all,
                  std::to_string(scan::never_touch().size()) + " entries");
            check("never-touch: fixes/ inside a build dir is still Protected",
                  scan::classify_dir("a/build/fixes", Kind::Regenerable, false).kind == Kind::Protected);
            check("never-touch: below a protected dir stays Protected",
                  scan::classify_dir(".ssh/x/build", Kind::Protected, true).kind == Kind::Protected);
        }

        check("human_bytes: 0 B / 999 B / 1.5 KB / 340 MB / 2.0 GB",
              scan::human_bytes(0) == "0 B" && scan::human_bytes(999) == "999 B" &&
              scan::human_bytes(1500) == "1.5 KB" && scan::human_bytes(340'000'000) == "340 MB" &&
              scan::human_bytes(2'000'000'000) == "2.0 GB");

        fs::remove_all(home, ec);
    }
    // -- Preflight: mountinfo, the checklist's branches, gather on a temp home (s003)
    {
        std::cout << "\n-- preflight --\n";
        namespace pf = core::preflight;
        using pf::Status;
        // Scott's MBP layout (Fedora btrfs: / is subvol root, /home its own),
        // plus a decoy prefix and an escaped space.
        const std::string mi =
            "60 1 0:33 /root / rw,relatime shared:1 - btrfs /dev/nvme0n1p6 rw,subvol=/root\n"
            "90 60 0:33 /home /home rw,relatime shared:50 - btrfs /dev/nvme0n1p6 rw,subvol=/home\n"
            "95 60 8:17 / /homework rw shared:2 - ext4 /dev/sdb1 rw\n"
            "100 90 0:50 / /home/scott/My\\040Drive rw nosuid - fuse.rclone gdrive: rw\n"
            "garbage line\n";
        const auto mounts = pf::parse_mountinfo(mi);
        check("mountinfo: four lines parsed, garbage skipped", mounts.size() == 4, std::to_string(mounts.size()));
        const auto* h = pf::find_mount(mounts, "/home/scott");
        check("mountinfo: /home/scott -> /home, btrfs, subvol /home",
              h && h->mount_point == "/home" && h->fstype == "btrfs" && h->root == "/home" &&
              h->source == "/dev/nvme0n1p6");
        const auto* w = pf::find_mount(mounts, "/homework/x");
        check("mountinfo: /homework is not under /home", w && w->fstype == "ext4");
        const auto* d = pf::find_mount(mounts, "/home/scott/My Drive/f");
        check("mountinfo: \\040 unescaped, the deepest mount wins", d && d->fstype == "fuse.rclone");
        const auto* r = pf::find_mount(mounts, "/etc");
        check("mountinfo: anything else falls to /", r && r->mount_point == "/");

        const auto status_of = [](const std::vector<pf::Check>& cs, const std::string& id) {
            for (const auto& c : cs) if (c.id == id) return c.status;
            return Status::Red;
        };
        constexpr std::uint64_t GB = 1000ull * 1000 * 1000;
        pf::Inputs good;
        good.home = "/home/scott";
        good.mount = *h;
        good.pika_age_s = 2 * 86400;
        good.free_bytes = 500 * GB;
        good.total_bytes = 1000 * GB;
        auto cs = pf::evaluate(good);
        check("checklist: four rows (repos waits for s006)", cs.size() == 4);
        check("checklist: own btrfs subvolume is green", status_of(cs, "filesystem") == Status::Green);
        check("checklist: snapshot is amber with none taken", status_of(cs, "snapshot") == Status::Amber);
        {   // s013: the snapshot row
            const auto detail_of = [](const std::vector<pf::Check>& v) { for (const auto& c : v) if (c.id == "snapshot") return c.detail; return std::string(); };
            pf::Inputs sn = good;
            sn.snap_possible = true; sn.helper = true; sn.snap_dir = "/home/.rouster-snapshots";
            sn.snap_newest = "home-20261106-120000"; sn.snap_count = 3;
            sn.snap_age_s = 2 * 3600;
            const auto fresh = pf::evaluate(sn);
            check("snapshot row: one 2 hours old -> green, names it", status_of(fresh, "snapshot") == Status::Green &&
                  detail_of(fresh).find("home-20261106-120000") != std::string::npos, detail_of(fresh));
            sn.snap_age_s = 30 * 3600;
            check("snapshot row: 30 hours old -> amber, says when", status_of(pf::evaluate(sn), "snapshot") == Status::Amber &&
                  detail_of(pf::evaluate(sn)).find("30 hours") != std::string::npos, detail_of(pf::evaluate(sn)));
            sn.snap_age_s = -86400;   // a clock that went back: not green on a future stamp
            check("snapshot row: a stamp from tomorrow is not fresh", status_of(pf::evaluate(sn), "snapshot") == Status::Amber);
            sn.snap_age_s.reset(); sn.helper = false;
            check("snapshot row: no helper -> amber, names install.sh --helper",
                  detail_of(pf::evaluate(sn)).find("install.sh --helper") != std::string::npos, detail_of(pf::evaluate(sn)));
            sn.snap_possible = false; sn.snap_why = "home shares the subvolume mounted at /";
            check("snapshot row: can't be taken -> amber with the why", status_of(pf::evaluate(sn), "snapshot") == Status::Amber &&
                  detail_of(pf::evaluate(sn)).find("mounted at /") != std::string::npos);
            sn.mount = *w;   // ext4
            sn.snap_possible = true; sn.helper = true; sn.snap_age_s = 60;
            check("snapshot row: never green off btrfs, never red", status_of(pf::evaluate(sn), "snapshot") == Status::Amber);
        }
        check("checklist: Pika 2 days ago is green", status_of(cs, "pika") == Status::Green);
        check("checklist: 500 GB free is green", status_of(cs, "free") == Status::Green);

        pf::Inputs shared = good;
        shared.mount = *pf::find_mount(mounts, "/");
        check("checklist: home on the system subvolume is amber",
              status_of(pf::evaluate(shared), "filesystem") == Status::Amber);

        pf::Inputs stale = good;
        stale.pika_age_s = 8 * 86400;
        check("checklist: Pika 8 days on btrfs is amber", status_of(pf::evaluate(stale), "pika") == Status::Amber);
        stale.mount = *w;   // ext4: Pika is the only net
        auto ext = pf::evaluate(stale);
        check("checklist: ext4 is amber, and stale Pika goes red",
              status_of(ext, "filesystem") == Status::Amber && status_of(ext, "pika") == Status::Red);
        pf::Inputs none = good;
        none.pika_age_s.reset();
        check("checklist: no Pika history on btrfs is amber", status_of(pf::evaluate(none), "pika") == Status::Amber);

        pf::Inputs tight = good;
        tight.free_bytes = 1 * GB;
        check("checklist: 1 GB free is red", status_of(pf::evaluate(tight), "free") == Status::Red);
        tight.free_bytes = 5 * GB;
        check("checklist: 5 GB free is amber", status_of(pf::evaluate(tight), "free") == Status::Amber);
        tight.total_bytes = 0;
        check("checklist: unreadable free space is red", status_of(pf::evaluate(tight), "free") == Status::Red);
        pf::Inputs lost = good;
        lost.mount.reset();
        check("checklist: unknown filesystem is amber", status_of(pf::evaluate(lost), "filesystem") == Status::Amber);

        // gather() on a temp home: finds a Pika history by its mtime, reads
        // free space, and changes nothing.
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_preflight";
        std::error_code ec;
        fs::remove_all(home, ec);
        put(home, ".var/app/org.gnome.World.PikaBackup/config/pika-backup/history.json", 100);
        const fs::path hist = home / ".var/app/org.gnome.World.PikaBackup/config/pika-backup/history.json";
        fs::last_write_time(hist, fs::file_time_type::clock::now() - std::chrono::hours(72), ec);
        const std::string before = tree_snapshot(home);
        const auto in = pf::gather(home.string());
        check("gather: Pika's Flatpak history found, ~3 days old",
              in.pika_age_s && *in.pika_age_s > 71 * 3600 && *in.pika_age_s < 73 * 3600 && in.pika_where == hist.string(),
              in.pika_age_s ? pf::human_age(*in.pika_age_s) : "none");
        check("gather: a mount and free space were read", in.mount.has_value() && in.total_bytes > 0,
              in.mount ? in.mount->fstype + " on " + in.mount->mount_point : "no mount");
        check("gather: nothing on disk changed", tree_snapshot(home) == before);
        fs::remove_all(home, ec);

        check("human_age: just now / 5 minutes / 3 hours / 44 hours / 9 days",
              pf::human_age(30) == "just now" && pf::human_age(300) == "5 minutes" &&
              pf::human_age(3 * 3600) == "3 hours" && pf::human_age(86400 + 3600 * 20) == "44 hours" &&
              pf::human_age(9 * 86400) == "9 days");
    }
    // -- Helper: what the app and the root helper agree on (s013) -------------
    {
        std::cout << "\n-- helper --\n";
        namespace hp = core::helper;
        check("helper: snapshot:5 / :1 / :20 read", hp::parse("snapshot:5") == hp::Request{hp::Op::Snapshot, 5} &&
              hp::parse("snapshot:1") && hp::parse("snapshot:20"));
        bool none = true;
        for (const char* bad : {"snapshot", "snapshot:", "snapshot:0", "snapshot:21", "snapshot:-1", "snapshot:+5",
                                "snapshot:5:5", "snapshot: 5", "snapshot:5 ", "Snapshot:5", "snapshot:5;rm -rf /",
                                "snapshot:0005", "dnf:1", "", ":5", "snapshot:1e2", "../snapshot:5"})
            if (hp::parse(bad)) { none = false; std::cout << "  (read: " << bad << ")\n"; }
        check("helper: anything else is not a request (17 tries)", none);
        check("helper: text <-> parse", hp::text({hp::Op::Snapshot, 7}) == "snapshot:7" &&
              hp::parse(hp::text({hp::Op::Snapshot, 7})) == hp::Request{hp::Op::Snapshot, 7});
        const std::time_t t0 = 1794000000;   // a Nov 2026 second
        const std::string n0 = hp::snapshot_name(t0);
        check("helper: a snapshot name reads back to its second", hp::is_snapshot_name(n0) && hp::snapshot_time(n0) == t0, n0);
        bool names_ok = true;
        for (const char* bad : {"home-2026110-173400", "home-20261106_173400", "home-20261106-17340x", "x-20261106-173400",
                                "home-20261106-1734000", ".rouster", "home-20261106-173400/..", "my-photos"})
            if (hp::is_snapshot_name(bad)) names_ok = false;
        check("helper: other names are never Rouster's", names_ok);
        const std::vector<std::string> have = {"home-20261101-090000", "keep-me", "home-20261106-090000",
                                               "home-20261103-090000", "home-20261102-090000", "home-20261105-090000",
                                               "home-20261104-090000", "lost+found"};
        const auto gone = hp::to_expire(have, 5);
        check("helper: keep 5 of 6 -> the oldest goes, other names never", gone == std::vector<std::string>{"home-20261101-090000"},
              gone.empty() ? "none" : gone.front());
        check("helper: keep 2 -> four go, newest kept", hp::to_expire(have, 2).size() == 4 &&
              hp::to_expire(have, 2).front() == "home-20261104-090000");
        check("helper: keep 20 / keep 0 (read as 1)", hp::to_expire(have, 20).empty() && hp::to_expire(have, 0).size() == 5);
        const hp::Line l{"snapshot", "ok", "took /home/.rouster-snapshots/home-x\tand\nmore"};
        const auto back = hp::parse_line(hp::format_line(l));
        check("helper: a result line round-trips, tabs/newlines flattened", back && back->op == "snapshot" &&
              back->status == "ok" && back->detail == "took /home/.rouster-snapshots/home-x and more");
        const auto lines = hp::parse_output("pkexec: junk\n" + hp::format_line({"snapshot", "failed", "why"}) +
                                            "\nrouster-helper\tsnapshot\tmaybe\tx\n" + hp::format_line({"snapshot", "dry", "would"}) + "\n");
        check("helper: output -> only well-formed lines (junk, a bad status skipped)", lines.size() == 2 &&
              lines[0].status == "failed" && lines[1].status == "dry");
        const fs::path tmp = fs::temp_directory_path() / "rouster_selftest_place";
        std::error_code ec;
        fs::create_directories(tmp, ec);
        const auto pl = hp::place_for(tmp.string());
        struct statfs sfs{};
        ::statfs(tmp.c_str(), &sfs);
        const bool tmp_btrfs = static_cast<unsigned long>(sfs.f_type) == 0x9123683EUL;
        check("helper: place_for answers without root (" + std::string(tmp_btrfs ? "btrfs" : "not btrfs") + ")",
              tmp_btrfs ? (pl.ok || !pl.why.empty()) : (!pl.ok && pl.why == "home is not on btrfs"), pl.ok ? pl.dir : pl.why);
        check("helper: place_for on a missing folder -> no", !hp::place_for((tmp / "nope").string()).ok);
        check("helper: list_snapshots on a missing folder -> empty", hp::list_snapshots((tmp / "nope").string()).empty());
        fs::remove_all(tmp, ec);
    }
    // -- Door: the one door, on a fixture home (s004) --------------------------
    {
        std::cout << "\n-- door --\n";
        namespace dr = core::door;
        using dr::Verb;
        const fs::path home = share_base() / "rouster_selftest_door";
        std::error_code ec;
        fs::remove_all(home, ec);
        make_fixture(home);
        const std::string H = home.string();
        const std::string data = H + "/.local/share/rouster";
        dr::Door door(H, data);
        check("door: load with no manifest -- empty, nothing to undo", door.load() && !door.can_undo());

        const auto plan = [](std::string label, std::vector<dr::Action> a, bool previewed = true) {
            return dr::Plan{std::move(label), std::move(a), previewed};
        };
        const auto trash = [](std::string p) { return dr::Action{Verb::Trash, std::move(p), "", "test", 0}; };
        const auto move  = [](std::string a, std::string b) { return dr::Action{Verb::Move, std::move(a), std::move(b), "test", 0}; };
        const auto there = [](const std::string& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0; };

        // Preview first.
        {
            const std::string before = files_snapshot(home);
            const auto r = door.run(plan("x", {trash(H + "/Documents/report.pdf")}, false));
            check("door: an unpreviewed plan is refused, nothing moves",
                  !r.ok && r.done == 0 && files_snapshot(home) == before && !there(data + "/manifest.jsonl"));
        }

        // The verb table, walked: through the door, one step, undo -> the exact tree.
        check("gate: the verb table holds every verb", dr::verbs().size() == dr::kVerbCount);
        fs::create_directories(home / "Documents/archive", ec);
        put_bytes(home, "Documents/twin-a.bin", noise(70000, 7));   // Dedupe's pair (s008)
        put_bytes(home, "Documents/twin-b.bin", noise(70000, 7));
        const bool can_share = core::dups::can_share(H);
        // Clone's source: a bare repo OUTSIDE home (s007b).
        const fs::path gate_remote = fs::temp_directory_path() / "rouster_selftest_door_remote";
        fs::remove_all(gate_remote, ec);
        git_isolate();
        fs::create_directories(gate_remote / "work", ec);
        put(gate_remote, "work/a.txt", 10);
        sh(gate_remote.string(), "git -c init.defaultBranch=main init -q --bare r.git && cd work && "
           "git -c init.defaultBranch=main init -q && git add -A && git commit -qm a && git push -q ../r.git HEAD:main");
        // A created thing's undo sends it to the Trash: "exactly" is judged outside the Trash.
        const auto outside_trash = [](const std::string& snap) {
            std::istringstream in(snap);
            std::string out;
            for (std::string l; std::getline(in, l);)
                if (l.find("/.local/share/Trash/") == std::string::npos) out += l + "\n";
            return out;
        };
        auto fake_dconf = std::make_shared<FakeDconf>();
        fake_dconf->keys = {{"/org/x/a", "1"}, {"/org/y/c", "'z'"}};
        door.set_dconf([fake_dconf](const std::vector<std::string>& a, const std::string& in) { return (*fake_dconf)(a, in); });
        for (Verb v : dr::verbs()) {
            const std::string name = dr::verb_name(v);
            const std::string before = files_snapshot(home);
            const std::size_t batches = door.batches().size();
            dr::Action a;
            switch (v) {
                case Verb::Trash: a = trash(H + "/.cache/mozilla"); break;
                case Verb::Move:  a = move(H + "/Documents/report.pdf", H + "/Documents/archive/report.pdf"); break;
                case Verb::Write: a.verb = v; a.to = H + "/Documents/RESTORE.txt"; a.content = "url: x\n"; break;
                case Verb::Clone: a.verb = v; a.to = H + "/Documents/clone"; a.url = (gate_remote / "r.git").string();
                                  a.branch = "main"; break;
                case Verb::MakeDir: a.verb = v; a.to = H + "/Documents/made"; break;
                case Verb::Copy: a.verb = v; a.from = H + "/Documents/report.pdf"; a.to = H + "/Documents/report v2.pdf"; break;
                case Verb::Dedupe: a.verb = v; a.from = H + "/Documents/twin-a.bin"; a.to = H + "/Documents/twin-b.bin"; break;
                case Verb::Settings: a.verb = v; a.content = "[org/x]\na=2\nb=3\n"; break;
                case Verb::Root: a.verb = v; a.content = "snapshot:5"; break;
            }
            if (v == Verb::Root) {
                // Runs as root through pkexec -- here a fake helper that says what it did.
                std::vector<std::vector<std::string>> argvs;
                door.set_root([&argvs](const std::vector<std::string>& av) {
                    argvs.push_back(av);
                    core::vcs::Run x;
                    x.code = 0;
                    x.out = core::helper::format_line({"snapshot", "ok", "took /home/.rouster-snapshots/home-20261106-120000"}) + "\n";
                    return x;
                }, false);
                const auto r = door.run(plan(name + " one", {a}));
                check("gate/root: done through the door", r.ok && r.done == 1, r.messages.empty() ? "" : r.messages.front());
                check("gate/root: one step", door.batches().size() == batches + 1 && door.undo_label() == name + " one");
                check("gate/root: argv is pkexec + the helper's fixed path + run + the one request word",
                      argvs.size() == 1 && argvs[0] == std::vector<std::string>{"/usr/bin/pkexec", core::helper::kHelperPath, "run", "snapshot:5"});
                const auto* rec = door.record(door.batches().back().records.front());
                check("gate/root: what the helper said is kept as the note", rec && rec->note.find("home-20261106-120000") != std::string::npos);
                check("gate/root: no user file changed", files_snapshot(home) == before);
                const auto u = door.undo();
                check("gate/root: undo closes the step, runs nothing, changes nothing",
                      u.ok && u.done == 1 && argvs.size() == 1 && files_snapshot(home) == before);
                continue;
            }
            if (v == Verb::Settings) {
                // Changes settings, not files: the user's files stay; Rouster's records gain the two saved dumps.
                const auto keys_before = fake_dconf->keys;
                const auto outside_data = [&](const std::string& snap) {
                    std::istringstream in(snap);
                    std::string o;
                    for (std::string l; std::getline(in, l);) if (l.find("/.local/share/rouster") == std::string::npos) o += l + "\n";
                    return o;
                };
                const auto r = door.run(plan(name + " one", {a}));
                check("gate/settings: done through the door", r.ok && r.done == 1, r.messages.empty() ? "" : r.messages.front());
                check("gate/settings: one step", door.batches().size() == batches + 1 && door.undo_label() == name + " one");
                const auto* rec = door.record(door.batches().back().records.front());
                struct stat bs{};
                check("gate/settings: loaded (a=2, b=3 new, c kept); what they were saved first, 0600, in Rouster's records",
                      fake_dconf->keys.at("/org/x/a") == "2" && fake_dconf->keys.at("/org/x/b") == "3" &&
                      fake_dconf->keys.at("/org/y/c") == "'z'" && rec && rec->from.rfind(data + "/settings-", 0) == 0 &&
                      ::lstat(rec->from.c_str(), &bs) == 0 && (bs.st_mode & 0777) == 0600 &&
                      fake_dconf->calls.size() == 2 && fake_dconf->calls[0] == "dump" && fake_dconf->calls[1] == "load");
                check("gate/settings: no user file changed", outside_data(files_snapshot(home)) == outside_data(before));
                const auto u = door.undo();
                check("gate/settings: undo -> b reset, a back to 1 -- the settings exactly as before",
                      u.ok && fake_dconf->keys == keys_before, u.messages.empty() ? "" : u.messages.front());
                continue;
            }
            if (v == Verb::Dedupe) {
                // Moves nothing: the tree is the same before and after; the blocks are what change.
                const auto r = door.run(plan(name + " one", {a}));
                if (!can_share) {
                    check("gate/dedupe: (no btrfs at " + H + ") fails cleanly, tree exact",
                          !r.ok && r.failed == 1 && files_snapshot(home) == before,
                          r.messages.empty() ? "" : r.messages.front());
                    door.undo();
                    continue;
                }
                check("gate/dedupe: done through the door", r.ok && r.done == 1, r.messages.empty() ? "" : r.messages.front());
                check("gate/dedupe: one step", door.batches().size() == batches + 1 && door.undo_label() == name + " one");
                check("gate/dedupe: blocks shared, every path / size / mtime / inode unchanged",
                      core::dups::same_blocks(a.from, a.to) && files_snapshot(home) == before);
                const auto u = door.undo();
                check("gate/dedupe: undo closes the step, tree exact", u.ok && files_snapshot(home) == before,
                      u.messages.empty() ? "" : u.messages.front());
                continue;
            }
            const auto r = door.run(plan(name + " one", {a}));
            check("gate/" + name + ": done through the door", r.ok && r.done == 1,
                  r.messages.empty() ? "" : r.messages.front());
            check("gate/" + name + ": one step", door.batches().size() == batches + 1 && door.undo_label() == name + " one");
            check("gate/" + name + ": the tree changed", files_snapshot(home) != before);
            const auto u = door.undo();
            const bool exact = dr::creates(v) ? outside_trash(files_snapshot(home)) == outside_trash(before)
                                              : files_snapshot(home) == before;
            check("gate/" + name + ": undo puts it back exactly", u.ok && exact,
                  u.messages.empty() ? "" : u.messages.front());
        }
        check("gate: write -- the text landed, staging gone, undo sent it to the Trash",
              fs::exists(home / ".local/share/Trash/files/RESTORE.txt") && !fs::exists(home / "Documents/RESTORE.txt"));
        check("gate: clone -- undo sent the clone to the Trash, no staging left behind",
              fs::exists(home / ".local/share/Trash/files/clone/a.txt") && !fs::exists(home / "Documents/clone"));
        {
            bool stray = false;
            for (const auto& e : fs::directory_iterator(home / "Documents"))
                if (e.path().filename().string().rfind(".rouster-", 0) == 0) stray = true;
            check("gate: no .rouster- staging names left", !stray);
        }
        {   // s013: the Root verb's policy, its failures, the manifest, a crash
            const auto rt = [](std::string req) { dr::Action a; a.verb = dr::Verb::Root; a.content = std::move(req); return a; };
            dr::Action named = rt("snapshot:5");
            named.from = H + "/Documents";
            check("door/root: refused -- a file named, an unknown word, out of range, a shell tail",
                  !door.refusal(named).empty() && !door.refusal(rt("rm:1")).empty() && !door.refusal(rt("snapshot:99")).empty() &&
                  !door.refusal(rt("snapshot:5; reboot")).empty() && door.refusal(rt("snapshot:3")).empty());
            int calls = 0;
            const auto fake = [&calls](int code, std::string out, std::string err) {
                return [&calls, code, out, err](const std::vector<std::string>&) {
                    ++calls;
                    core::vcs::Run x; x.code = code; x.out = out; x.err = err; return x;
                };
            };
            const std::string before = files_snapshot(home);
            calls = 0;
            door.set_root(fake(126, "", "Error executing command as another user: Not authorized"), false);
            auto r = door.run(plan("snap", {rt("snapshot:5")}));
            check("door/root: the password prompt dismissed (126) -> failed, says so, nothing changed",
                  !r.ok && r.failed == 1 && calls == 1 && !r.messages.empty() &&
                  r.messages.front().find("not authorised") != std::string::npos && files_snapshot(home) == before,
                  r.messages.empty() ? "" : r.messages.front());
            door.undo();
            door.set_root(fake(127, "", "Error executing command as another user: Not authorized\n\nThis incident has been reported.\n"), false);
            r = door.run(plan("snap", {rt("snapshot:5")}));
            check("door/root: polkit says no (127, Not authorized -- seen in the sandbox) -> 'not authorised'", !r.ok &&
                  !r.messages.empty() && r.messages.front().find("not authorised") != std::string::npos, r.messages.empty() ? "" : r.messages.front());
            door.undo();
            door.set_root(fake(1, core::helper::format_line({"snapshot", "failed", "home is not on btrfs"}) + "\n", ""), false);
            r = door.run(plan("snap", {rt("snapshot:5")}));
            check("door/root: the helper's own failure is the message", !r.ok && !r.messages.empty() &&
                  r.messages.front().find("home is not on btrfs") != std::string::npos, r.messages.empty() ? "" : r.messages.front());
            door.undo();
            door.set_root(fake(0, "", ""), false);
            r = door.run(plan("snap", {rt("snapshot:5")}));
            check("door/root: exit 0 but no result line -> failed (never assumed)", !r.ok && r.failed == 1);
            door.undo();
            std::vector<std::string> seen;
            door.set_root([&seen](const std::vector<std::string>& av) {
                seen = av;
                core::vcs::Run x; x.code = 0;
                x.out = core::helper::format_line({"snapshot", "dry", "would take /home/.rouster-snapshots/home-x"}) + "\n";
                return x;
            }, true);
            r = door.run(plan("snap dry", {rt("snapshot:5")}));
            check("door/root: a fixture door asks for --dry-run", r.ok && seen.size() == 5 && seen[3] == "--dry-run");
            dr::Door again(H, data);
            again.load();
            const dr::Record* last = nullptr;
            for (const auto& b : again.batches()) if (b.label == "snap dry") last = again.record(b.records.front());
            check("door/root: the note survives a reload", last && last->state == dr::State::Done &&
                  last->note.find("would take") != std::string::npos);
            door.undo();
            // A crash after the intent: the helper ran or didn't -- it can't be looked at.
            {
                std::ofstream m(data + "/manifest.jsonl", std::ios::app);
                m << R"({"t":"batch","batch":800,"label":"crash root","when":1})" << "\n"
                  << R"({"t":"intent","id":8000,"batch":800,"verb":"root","from":"","to":"snapshot:5","when":1})" << "\n";
            }
            dr::Door crashed(H, data);
            crashed.load();
            bool noted = false;
            for (const auto& u : crashed.unfinished()) if (u.find("root snapshot:5") != std::string::npos) noted = true;
            const auto* cr = crashed.record(8000);
            check("door/root: a crash after the intent -> done, noted 'may or may not'", cr && cr->state == dr::State::Done && noted);
            const auto cu = crashed.undo();
            check("door/root: ...and its undo just closes it", cu.ok);
            door.load();
        }
        {   // s012d: the Settings verb's policy, a failed load, a crash
            const auto set = [](std::string ini) { dr::Action a; a.verb = dr::Verb::Settings; a.content = std::move(ini); return a; };
            dr::Action named = set("[a]\nk=1\n");
            named.to = H + "/Documents/x";
            check("door/settings: refused -- a file named, not a dump, a key before any section, empty",
                  !door.refusal(named).empty() && !door.refusal(set("hello world\n")).empty() &&
                  !door.refusal(set("k=1\n[a]\n")).empty() && !door.refusal(set("")).empty() &&
                  !door.refusal(set("[a/../b]\nk=1\n")).empty() && door.refusal(set("# c\n[a]\nk=1\n\n")).empty());
            check("door/settings: keys -- sections, the root, comments skipped",
                  dr::dconf_keys("[/]\nr=1\n[org/a]\n# no\nk=2\nj='x=y'\n") ==
                      std::vector<std::string>{"/r", "/org/a/k", "/org/a/j"});
            const auto keys0 = fake_dconf->keys;
            fake_dconf->fail_load = true;
            const auto f = door.run(dr::Plan{"settings fail", {set("[org/x]\na=9\n")}, true});
            fake_dconf->fail_load = false;
            check("door/settings: a failed load -> failed, said, settings unchanged",
                  !f.ok && f.failed == 1 && !f.messages.empty() && f.messages[0].find("GNOME settings") != std::string::npos &&
                  fake_dconf->keys == keys0, f.messages.empty() ? "" : f.messages.front());
            door.undo();
            // A crash after the intent: the manifest has no outcome line.
            const auto ok = door.run(dr::Plan{"settings crash", {set("[org/x]\na=7\nz=1\n")}, true});
            {
                std::ifstream in(data + "/manifest.jsonl");
                std::string all, l, kept;
                while (std::getline(in, l)) all += l + "\n";
                const auto cut = all.rfind("{\"id\"");   // the last line: its "done"
                kept = all.substr(0, cut);
                std::ofstream o(data + "/manifest.jsonl", std::ios::trunc);
                o << kept;
            }
            dr::Door again(H, data);
            again.set_dconf([fake_dconf](const std::vector<std::string>& a, const std::string& in) { return (*fake_dconf)(a, in); });
            again.load();
            bool noted = false;
            for (const auto& u : again.unfinished()) if (u.find("settings load") != std::string::npos) noted = true;
            const auto u = again.undo();
            check("door/settings: a crash mid-load -> counted done, noted; Undo puts the saved settings back",
                  ok.ok && noted && u.ok && fake_dconf->keys == keys0, u.messages.empty() ? "" : u.messages.front());
            door.load();
        }
        {   // create-verb policy
            dr::Action w; w.verb = Verb::Write; w.to = H + "/Documents/report.pdf"; w.content = "x";
            dr::Action big; big.verb = Verb::Write; big.to = H + "/Documents/big.txt"; big.content.assign(dr::kWriteMax + 1, 'x');
            dr::Action prot; prot.verb = Verb::Write; prot.to = H + "/.ssh/RESTORE.txt";
            dr::Action out; out.verb = Verb::Write; out.to = "/tmp/rouster_out.txt";
            dr::Action inj; inj.verb = Verb::Clone; inj.to = H + "/Documents/c2"; inj.url = "--upload-pack=touch /tmp/x";
            dr::Action badc; badc.verb = Verb::Clone; badc.to = H + "/Documents/c3"; badc.url = "u"; badc.commit = "zz";
            dr::Action src; src.verb = Verb::Write; src.from = H + "/Documents"; src.to = H + "/Documents/n.txt";
            check("door: create verbs refuse -- an existing path, too big, protected, outside home, a flag as URL, a bad commit, a source",
                  !door.refusal(w).empty() && !door.refusal(big).empty() && !door.refusal(prot).empty() &&
                  !door.refusal(out).empty() && !door.refusal(inj).empty() && !door.refusal(badc).empty() &&
                  !door.refusal(src).empty(), door.refusal(inj));
            dr::Action gone; gone.verb = Verb::Clone; gone.to = H + "/Documents/c4"; gone.url = (gate_remote / "nope.git").string();
            const std::string b4 = outside_trash(files_snapshot(home));
            const auto r = door.run(plan("bad clone", {gone}));
            check("door: a clone that fails leaves nothing behind (outside the Trash)", !r.ok && r.failed == 1 &&
                  outside_trash(files_snapshot(home)) == b4, r.messages.empty() ? "" : r.messages.front());
            door.undo();
        }
        fs::remove_all(gate_remote, ec);
        check("gate: nothing left to undo", !door.can_undo());

        // Trash follows the freedesktop layout.
        {
            const std::string f = H + "/Documents/report.pdf";
            dr::Identity id0{};
            dr::identity_of(f, id0);
            const auto r = door.run(plan("Trash report", {trash(f)}));
            const dr::Record* rec = door.record(door.batches().back().records.front());
            std::ifstream info(rec->info);
            std::stringstream ib;
            ib << info.rdbuf();
            dr::Identity id1{};
            check("trash: gone from its place, in Trash/files, same file",
                  r.ok && !there(f) && rec->to == H + "/.local/share/Trash/files/report.pdf" &&
                  dr::identity_of(rec->to, id1) && id1 == id0);
            check("trash: the .trashinfo names where it came from",
                  ib.str().find("Path=" + f + "\n") != std::string::npos &&
                  ib.str().find("DeletionDate=") != std::string::npos, rec->info);
            door.undo();
            check("trash: undo -- back, and the .trashinfo removed", there(f) && !there(rec->info));
        }
        check("trashinfo: a space is %20", dr::trashinfo("/h/My File", 0).find("Path=/h/My%20File\n") != std::string::npos);

        // Two of the same name in one batch: the second gets .2; undo restores both.
        {
            put(home, "Documents/dup.txt", 10);
            put(home, "Pictures/dup.txt", 20);
            const std::string before = files_snapshot(home);
            const auto r = door.run(plan("Two dups", {trash(H + "/Documents/dup.txt"), trash(H + "/Pictures/dup.txt")}));
            check("trash: same names, both in, the second as dup.txt.2",
                  r.ok && there(H + "/.local/share/Trash/files/dup.txt") && there(H + "/.local/share/Trash/files/dup.txt.2"));
            const auto u = door.undo();
            check("trash: one undo puts both back", u.ok && u.done == 2 && files_snapshot(home) == before);
        }

        // Refusals: each one leaves the tree as it was.
        {
            const std::string before = files_snapshot(home);
            const std::pair<std::string, dr::Action> refuse[] = {
                {"~/.ssh (never-touch)",           trash(H + "/.ssh/id_ed25519")},
                {"~/.config/dconf (never-touch)",  trash(H + "/.config/dconf/user")},
                {"a fixes/ folder",                trash(H + "/Develop/proj/fixes")},
                {"inside a fixes/ folder",         trash(H + "/Develop/proj/fixes/patch.diff")},
                {"outside home",                   trash("/etc/hostname")},
                {"home itself",                    trash(H)},
                {"a relative path",                trash("Documents/report.pdf")},
                {"a path with ..",                 trash(H + "/Documents/../Pictures/a.jpg")},
                {"through a symlink (to /etc)",    trash(H + "/Documents/etc/hostname")},
                {"through a symlink (a cycle)",    trash(H + "/Documents/loop/Pictures/a.jpg")},
                {"already in the Trash",           trash(H + "/.local/share/Trash/files/old.txt")},
                {"Rouster's own records",          trash(data)},
                {"something missing",              trash(H + "/Documents/nope.txt")},
                {"move onto an existing file",     move(H + "/Documents/report.pdf", H + "/Pictures/a.jpg")},
                {"move into a missing folder",     move(H + "/Documents/report.pdf", H + "/Nope/report.pdf")},
                {"move into the Trash",            move(H + "/Documents/report.pdf", H + "/.local/share/Trash/files/r.pdf")},
                {"move into ~/.ssh",               move(H + "/Documents/report.pdf", H + "/.ssh/report.pdf")},
                {"move a folder into itself",      move(H + "/Documents", H + "/Documents/archive/Documents")},
                {"move out of home",               move(H + "/Documents/report.pdf", "/tmp/report.pdf")},
            };
            for (const auto& [what, a] : refuse) {
                const auto r = door.run(plan("bad", {a}));
                check("refuse: " + what, !r.ok && r.done == 0 && !r.messages.empty(),
                      r.messages.empty() ? "NOT REFUSED" : r.messages.front());
            }
            const auto mixed = door.run(plan("mixed", {trash(H + "/Pictures/a.jpg"), trash(H + "/.ssh/id_ed25519")}));
            check("refuse: one bad action stops the whole plan before anything moves",
                  !mixed.ok && mixed.done == 0 && there(H + "/Pictures/a.jpg"));
            check("refuse: the tree is exactly as it was", files_snapshot(home) == before);
        }

        // The manifest survives: a fresh Door reads it and can undo what the old one did.
        {
            const std::string before = files_snapshot(home);
            door.run(plan("Trash a", {trash(H + "/Pictures/a.jpg")}));
            door.run(plan("Trash notes", {trash(H + "/Documents/build/notes.txt")}));
            dr::Door again(H, data);
            again.load();
            check("manifest: a new door sees the last step", again.can_undo() && again.undo_label() == "Trash notes",
                  again.undo_label());
            again.undo();
            check("manifest: undo order is newest first", again.undo_label() == "Trash a");
            again.undo();
            check("manifest: both undone, the tree is back", files_snapshot(home) == before && !again.can_undo());
            dr::Door third(H, data);
            third.load();
            check("manifest: undone stays undone across a reload", !third.can_undo());
        }

        // Undo never overwrites: something new at the old place -> it stays in the Trash.
        {
            door.load();
            const std::string f = H + "/Documents/report.pdf";
            door.run(plan("Trash report", {trash(f)}));
            put(home, "Documents/report.pdf", 7);   // the user made a new one
            const auto u = door.undo();
            const std::string t = H + "/.local/share/Trash/files/report.pdf";
            check("undo: a new file in the way is left alone; the old stays in the Trash",
                  !u.ok && u.failed == 1 && fs::file_size(f, ec) == 7 && there(t),
                  u.messages.empty() ? "" : u.messages.front());
            fs::remove(f, ec);                      // test cleanup, outside the door on purpose
            dr::Door d2(H, data);
            d2.load();
            check("undo: the failed undo is recorded; the item is still Done", !d2.can_undo() &&
                  d2.record(d2.batches().back().records.front())->state == dr::State::Done);
            fs::rename(t, f, ec);                   // put the fixture back by hand
            fs::remove(H + "/.local/share/Trash/info/report.pdf.trashinfo", ec);
        }

        // A crash between intent and outcome: load() looks and decides.
        {
            const std::string from = H + "/Pictures/a.jpg", to = H + "/Pictures/moved.jpg";
            dr::Identity id{};
            dr::identity_of(from, id);
            {
                std::ofstream m(data + "/manifest.jsonl", std::ios::app);
                m << R"({"t":"batch","batch":900,"label":"crashed","when":1})" << "\n";
                m << R"({"t":"intent","id":9000,"batch":900,"verb":"move","from":")" << from << R"(","to":")" << to
                  << R"(","info":"","when":1,"ident":{"dev":)" << id.dev << R"(,"ino":)" << id.ino << R"(,"size":)"
                  << id.size << R"(,"mtime_ns":)" << id.mtime_ns << R"(,"dir":false}})" << "\n";
                m << R"({"t":"don)";   // the torn last line
            }
            fs::rename(from, to, ec);             // the rename DID happen before the crash
            dr::Door d3(H, data);
            d3.load();
            check("crash: an intent whose item is at `to` reads as done",
                  d3.record(9000) && d3.record(9000)->state == dr::State::Done && d3.unfinished().size() == 1,
                  d3.unfinished().empty() ? "" : d3.unfinished().front());
            check("crash: and it can be undone", d3.undo_label() == "crashed" && d3.undo().ok && there(from) && !there(to));
        }

        // The cross-filesystem rule needs two filesystems; as root, mount one.
        if (::geteuid() == 0) {
            const fs::path mnt = home / "Mnt";
            fs::create_directory(mnt, ec);
            if (::system(("mount -t tmpfs none '" + mnt.string() + "' 2>/dev/null").c_str()) == 0) {
                put(home, "Mnt/far.txt", 10);
                const auto r = door.run(plan("far", {trash(H + "/Mnt/far.txt")}));
                check("exdev: another filesystem -- failed, not copied, still there",
                      !r.ok && r.failed == 1 && there(H + "/Mnt/far.txt") &&
                      !there(H + "/.local/share/Trash/files/far.txt") &&
                      !there(H + "/.local/share/Trash/info/far.txt.trashinfo"),
                      r.messages.empty() ? "" : r.messages.front());
                (void)::system(("umount '" + mnt.string() + "'").c_str());
            }
        }
        fs::remove_all(home, ec);
    }


    // -- Sweep: find (read-only) -> plan -> the door -> undo (s006) ----------
    {
        std::cout << "\n-- sweep --\n";
        namespace sw = core::sweep;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_sweep";
        const fs::path outside = fs::temp_directory_path() / "rouster_selftest_sweep_out";
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::remove_all(outside, ec);
        make_fixture(home);
        const std::string H = home.string();
        const std::int64_t now = 2000000000;                       // pinned clock
        const auto age = [&](const std::string& rel, int days) {   // set a file's mtime
            struct timespec ts[2] = {{now - days * 86400, 0}, {now - days * 86400, 0}};
            ::utimensat(AT_FDCWD, (home / rel).c_str(), ts, AT_SYMLINK_NOFOLLOW);
        };
        // Sweep's own extras on top of the shared fixture.
        put(home, ".cache/huggingface/model.bin", 9000);           // slow to rebuild: unchecked
        put(home, ".cache/fixes/keep.diff", 100);                  // any fixes/: never offered
        put(home, ".local/state/app/old.log", 4000);    age(".local/state/app/old.log", 60);
        put(home, ".local/state/app/new.log", 4000);    age(".local/state/app/new.log", 2);
        put(home, ".local/share/app2/x.log.1", 4000);   age(".local/share/app2/x.log.1", 90);
        put(home, ".local/share/app2/catalog", 4000);   age(".local/share/app2/catalog", 90);
        put(home, ".local/share/keyrings/k.log", 100);  age(".local/share/keyrings/k.log", 90);
        put(home, ".local/share/Trash/files/t.log", 100); age(".local/share/Trash/files/t.log", 90);
        put(outside, "far/far.log", 100);
        ::utimensat(AT_FDCWD, (outside / "far/far.log").c_str(),
                    std::array<timespec, 2>{timespec{now - 99 * 86400, 0}, timespec{now - 99 * 86400, 0}}.data(), 0);
        fs::create_directory_symlink(outside / "far", home / ".local/state/far", ec);   // never followed

        check("sweep: is_log_name",
              sw::is_log_name("a.log") && sw::is_log_name("Xorg.0.log.old") && sw::is_log_name("a.log.1") &&
              sw::is_log_name("a.log.gz") && !sw::is_log_name(".log") && !sw::is_log_name("catalog") &&
              !sw::is_log_name("a.logx") && !sw::is_log_name("a.log."));

        const std::string before = tree_snapshot(home);
        core::scan::Progress prog;
        const sw::Found f = sw::find(H, prog, now);
        check("sweep: find changes nothing", tree_snapshot(home) == before);
        check("sweep: find finishes, not cancelled, not refused", prog.done && !f.cancelled && f.refused.empty());

        const auto get = [&](const std::string& rel) -> const sw::Candidate* {
            for (const auto& c : f.items) if (c.rel == rel) return &c;
            return nullptr;
        };
        std::string listed;
        for (const auto& c : f.items) listed += c.rel + (c.checked ? "+ " : "- ");
        const auto* moz = get(".cache/mozilla");
        const auto* th  = get(".cache/thumbnails");
        const auto* hf  = get(".cache/huggingface");
        const auto* fl  = get(".var/app/org.example.Y/cache");
        const auto* ol  = get(".local/state/app/old.log");
        const auto* rl  = get(".local/share/app2/x.log.1");
        check("sweep: a cache is offered, pre-checked", moz && moz->checked && moz->group == sw::Group::Caches, listed);
        check("sweep: thumbnails are their own group, pre-checked", th && th->checked && th->group == sw::Group::Thumbnails);
        check("sweep: a slow-to-rebuild cache is offered UNchecked, with why", hf && !hf->checked && !hf->note.empty());
        check("sweep: a Flatpak app cache is offered, pre-checked", fl && fl->checked && fl->group == sw::Group::AppCaches);
        check("sweep: old logs are offered UNchecked (state + share, rotated name)",
              ol && rl && !ol->checked && !rl->checked && ol->group == sw::Group::Logs);
        check("sweep: never offered -- ~/.cache itself, a fixes/ in it, a build dir",
              !get(".cache") && !get(".cache/fixes") && !get("Develop/proj/rouster/build"));
        check("sweep: never offered -- a fresh log, a non-log, a log in keyrings or the Trash, a symlinked log dir",
              !get(".local/state/app/new.log") && !get(".local/share/app2/catalog") &&
              !get(".local/share/keyrings/k.log") && !get(".local/share/Trash/files/t.log") &&
              !get(".local/state/far/far.log"));
        bool ordered = true;
        for (std::size_t i = 1; i < f.items.size(); ++i) {
            const auto &a = f.items[i - 1], &b = f.items[i];
            if (a.group > b.group || (a.group == b.group && a.bytes < b.bytes)) ordered = false;
        }
        check("sweep: grouped in order, biggest first within a group", ordered);
        check("sweep: the Trash is measured (2 files), never offered", f.trash_files == 2 && f.trash_bytes > 0,
              std::to_string(f.trash_files) + " files");

        // Plan: only what is checked; unpreviewed until the UI says otherwise.
        sw::Found g = f;
        for (auto& c : g.items) if (c.rel == ".local/state/app/old.log") c.checked = true;   // the user ticks one log
        dr::Plan p = sw::plan(H, g.items);
        std::size_t want = 0;
        for (const auto& c : g.items) want += c.checked;
        bool shape = p.actions.size() == want && !p.previewed && p.label.rfind("Sweep: ", 0) == 0;
        for (const auto& a : p.actions)
            shape &= a.verb == dr::Verb::Trash && a.from.rfind(H + "/", 0) == 0 && a.to.empty() &&
                     a.from != H + "/.cache/huggingface" && a.from != H + "/.local/share/app2/x.log.1";
        check("sweep: the plan is exactly the checked items, as Trash, unpreviewed", shape, p.label);

        // Through the door and back.
        const std::string files_before = files_snapshot(home);
        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        const auto r0 = door.run(p);
        check("sweep: the door refuses the plan until it is previewed", !r0.ok && r0.done == 0 &&
              files_snapshot(home) == files_before);
        p.previewed = true;
        const auto r = door.run(p);
        bool gone = true, intrash = true;
        for (const auto& a : p.actions) {
            struct stat st{};
            gone &= ::lstat(a.from.c_str(), &st) != 0;
        }
        intrash = fs::exists(home / ".local/share/Trash/files/mozilla") &&
                  fs::exists(home / ".local/share/Trash/files/thumbnails") &&
                  fs::exists(home / ".local/share/Trash/files/old.log") &&
                  fs::exists(home / ".local/share/Trash/info/mozilla.trashinfo");
        check("sweep: run -- every checked item is in the Trash, one batch", r.ok && r.done == want && gone && intrash,
              std::to_string(r.done) + " done" + (r.messages.empty() ? "" : ", " + r.messages.front()));
        check("sweep: run -- the unchecked stayed", fs::exists(home / ".cache/huggingface/model.bin") &&
              fs::exists(home / ".local/share/app2/x.log.1") && fs::exists(home / ".cache/fixes/keep.diff") &&
              fs::is_directory(home / ".cache"));
        check("sweep: undo label names the sweep", door.can_undo() && door.undo_label() == p.label, door.undo_label());
        const auto u = door.undo();
        check("sweep: undo puts every file back exactly", u.ok && files_snapshot(home) == files_before,
              std::to_string(u.done) + " back");

        // A second find after undo sees the same thing again.
        core::scan::Progress prog2;
        const sw::Found f2 = sw::find(H, prog2, now);
        check("sweep: after undo, find offers the same items", f2.items.size() == f.items.size());

        // Cancel.
        core::scan::Progress prog3;
        prog3.cancel = true;
        const sw::Found f3 = sw::find(H, prog3, now);
        check("sweep: a cancelled find says so", f3.cancelled && prog3.done);

        // A home that isn't there.
        core::scan::Progress prog4;
        check("sweep: refuses a missing home", !sw::find(H + "/nope", prog4, now).refused.empty() && prog4.done);

        fs::remove_all(home, ec);
        fs::remove_all(outside, ec);
    }

    // -- Dev projects: find / verify / lint (read-only) -> plan -> door (s007) --
    {
        std::cout << "\n-- dev projects --\n";
        namespace dv = core::dev;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_dev";
        const fs::path remotes = fs::temp_directory_path() / "rouster_selftest_dev_remotes";
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::remove_all(remotes, ec);
        fs::create_directories(home, ec);
        const bool made = make_dev_fixture(home, remotes);
        check("dev: the fixture's repos were made (git ran)", made);
        const std::string H = home.string();

        {
            const auto v = dv::git(H, {"--version"}, 10);
            check("dev: git runs as a child, output captured", v.code == 0 && v.out.rfind("git version", 0) == 0, v.out);
            std::atomic<bool> stop{true};
            const auto c = dv::git(H, {"--version"}, 10, &stop);
            check("dev: a cancelled git call is killed, code -1", c.code == -1 && !c.timed_out);
        }
        check("dev: url_host",
              dv::url_host("git@github.com:scott8420/rouster.git") == "github.com" &&
              dv::url_host("https://github.com/scott8420/rouster") == "github.com" &&
              dv::url_host("ssh://git@gitlab.com:22/a/b.git") == "gitlab.com" &&
              dv::url_host("https://user@codeberg.org/a/b") == "codeberg.org" &&
              dv::url_host("/srv/git/x.git") == "this computer" && dv::url_host("file:///x") == "this computer",
              dv::url_host("ssh://git@gitlab.com:22/a/b.git"));
        {
            std::string app;
            const bool ws = dv::is_workspace(H + "/Develop/rouster_ws", app);
            std::string app2;
            const bool bk = dv::is_workspace(H + "/Develop/baker", app2);
            std::string app3;
            check("dev: the workspace signature (docs/RULES.md + scripts/*backup.sh + <app>/CMakeLists.txt)",
                  ws && app == "rouster" && bk && app2 == "baker" && !dv::is_workspace(H + "/Develop/slate", app3));
        }

        const std::string before = tree_snapshot(home) + tree_snapshot(remotes);
        core::scan::Progress prog;
        const dv::Found f = dv::find(H, prog);
        check("dev: find changes nothing -- not a file, not even .git/index", tree_snapshot(home) + tree_snapshot(remotes) == before);
        check("dev: find finishes, not cancelled, not refused", prog.done && !f.cancelled && f.refused.empty());
        check("dev: the dev roots that exist are listed", f.roots == std::vector<std::string>{"Develop"});

        const auto get = [&](const std::string& rel) -> const dv::Project* {
            for (const auto& p : f.projects) if (p.rel == rel) return &p;
            return nullptr;
        };
        std::string listed;
        for (const auto& p : f.projects) listed += p.rel + "=" + dv::status_name(p.repo.status) + " ";
        const auto* rw = get("Develop/rouster_ws");
        const auto* bk = get("Develop/baker");
        const auto* sl = get("Develop/slate");
        const auto* sc = get("Develop/tools/scratch");
        const auto* be = get("Develop/behind");
        const auto* st = get("Develop/stale");
        check("dev: six projects found, a plain folder is not one", f.projects.size() == 6 && rw && bk && sl && sc && be && st &&
              !get("Develop/misc/notes") && !get("Develop/misc"), listed);
        check("dev: kinds -- two workspaces, four repos", rw && bk && sl && rw->kind == dv::Kind::Workspace &&
              bk->kind == dv::Kind::Workspace && sl->kind == dv::Kind::Repo && rw->name == "rouster");

        const auto chk = [](const dv::Project* p, const std::string& n) -> const dv::Check* {
            if (!p) return nullptr;
            for (const auto& c : p->repo.checks) if (c.name == n) return &c;
            return nullptr;
        };
        if (rw) {
            bool all = rw->repo.checks.size() == 6;
            for (const auto& c : rw->repo.checks) all &= c.ok;
            check("dev: a clean, pushed workspace is Backed -- six checks, all green",
                  rw->repo.status == dv::Status::Backed && all && rw->repo.remote == "origin" &&
                  rw->repo.host == "this computer" && rw->repo.head.size() == 40 && rw->repo.branch == "main",
                  std::string(dv::status_name(rw->repo.status)) + (rw->repo.checks.empty() ? "" : " " + rw->repo.checks.front().detail));
            check("dev: ignored-but-present is listed (build/), not judged",
                  std::find(rw->repo.ignored.begin(), rw->repo.ignored.end(), "build/") != rw->repo.ignored.end());
        }
        if (bk) {
            const auto *c = chk(bk, "committed"), *p = chk(bk, "pushed"), *b = chk(bk, "branches"),
                       *s = chk(bk, "stashes"), *t = chk(bk, "tags"), *l = chk(bk, "live");
            check("dev: dirty every way -> NotClean, and each check says which",
                  bk->repo.status == dv::Status::NotClean && c && !c->ok && p && !p->ok && b && !b->ok && s && !s->ok &&
                  t && !t->ok && l && l->ok,
                  p ? p->detail + " | " + (b ? b->detail : "") : "");
            check("dev: ...the details name the culprits",
                  c && c->detail.find("build.sh") != std::string::npos && p && p->detail.find("main (1 ahead)") != std::string::npos &&
                  b && b->detail.find("side") != std::string::npos && t && t->detail.find("v0") != std::string::npos);
        }
        check("dev: no remote -> NoRemote (not backed up)", sl && sl->repo.status == dv::Status::NoRemote);
        check("dev: a remote that won't answer -> Unreachable, with why",
              sc && sc->repo.status == dv::Status::Unreachable && !sc->repo.error.empty(), sc ? sc->repo.error : "");
        check("dev: the remote moved on and it was fetched -> Backed (nothing here is missing)",
              be && be->repo.status == dv::Status::Backed, be ? std::string(dv::status_name(be->repo.status)) : "");
        {
            const auto* p = chk(st, "pushed");
            check("dev: the remote moved on, NOT fetched -> NotClean, 'fetch, then look again'",
                  st && st->repo.status == dv::Status::NotClean && p && !p->ok && p->detail.find("fetch") != std::string::npos,
                  p ? p->detail : "");
        }

        // Lint.
        const auto has = [](const dv::Project* p, dv::Tier t, const std::string& s) {
            if (!p) return false;
            for (const auto& d : p->drift) if (d.tier == t && d.what.find(s) != std::string::npos) return true;
            return false;
        };
        if (rw) {
            bool only_note = true;
            for (const auto& d : rw->drift) only_note &= d.tier == dv::Tier::Note;
            check("dev: lint -- the template workspace has no drift, fixes/ reported as a note",
                  only_note && has(rw, dv::Tier::Note, "fixes/ holds 1 file") && rw->fixes_files == 1,
                  rw->drift.empty() ? "" : rw->drift.front().what);
        }
        check("dev: lint -- Baker's drift: missing include/, backup.sh, .desktop.in in resources/, example id",
              has(bk, dv::Tier::Required, "baker/include/") && has(bk, dv::Tier::Naming, "scripts/backup.sh") &&
              has(bk, dv::Tier::Naming, "resources/io.github.example.Baker.desktop.in") &&
              has(bk, dv::Tier::Naming, "app id io.github.example.Baker"));
        check("dev: lint -- unknowns: baker/notes/ in the app, stray.txt at the top",
              has(bk, dv::Tier::Unknown, "baker/notes/") && has(bk, dv::Tier::Unknown, "stray.txt") &&
              !has(bk, dv::Tier::Unknown, "build"));

        // Build output.
        const auto build = [](const dv::Project* p, const std::string& rel) -> const dv::BuildDir* {
            if (!p) return nullptr;
            for (const auto& b : p->builds) if (b.rel == rel) return &b;
            return nullptr;
        };
        const auto* b1 = build(rw, "Develop/rouster_ws/rouster/build");
        const auto* b2 = build(rw, "Develop/rouster_ws/rouster/libs/foo/build");
        const auto* b3 = build(sl, "Develop/slate/build");
        check("dev: build output -- <app>/build and libs/*/build, offered, pre-checked, measured",
              b1 && b2 && b1->offered && b1->checked && b2->offered && b2->checked && b1->bytes >= 90000 && b2->bytes >= 30000 &&
              b1->bytes == du(home / "Develop/rouster_ws/rouster/build").bytes,
              b1 ? std::to_string(b1->bytes) + " vs du " + std::to_string(du(home / "Develop/rouster_ws/rouster/build").bytes) : "");
        check("dev: no remote -> its build/ is shown, never offered", b3 && !b3->offered && !b3->checked && !b3->note.empty());
        check("dev: a build/ git tracks is not build output", bk && bk->builds.empty());

        // Plan, door, undo.
        dr::Plan p = dv::plan(H, f.projects);
        bool shape = p.actions.size() == 2 && !p.previewed && p.label == "Dev projects: build output of 1 project";
        for (const auto& a : p.actions) shape &= a.verb == dr::Verb::Trash && a.from.rfind(H + "/Develop/rouster_ws/rouster/", 0) == 0;
        check("dev: the plan is the offered + checked build dirs, as Trash, unpreviewed", shape, p.label);
        {
            dv::Found g = f;
            for (auto& pr : g.projects) for (auto& b : pr.builds) b.checked = true;   // even ticked, slate's stays out
            check("dev: an un-offered dir never reaches the plan, even checked", dv::plan(H, g.projects).actions.size() == 2);
        }
        fs::create_directories(home / ".local/share/Trash/files", ec);   // a real home has one
        fs::create_directories(home / ".local/share/Trash/info", ec);
        const std::string files_before = files_snapshot(home);
        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        check("dev: the door refuses it unpreviewed", !door.run(p).ok && files_snapshot(home) == files_before);
        p.previewed = true;
        const auto r = door.run(p);
        check("dev: light clean -- both build dirs in the Trash, the sources stay",
              r.ok && r.done == 2 && !fs::exists(home / "Develop/rouster_ws/rouster/build") &&
              !fs::exists(home / "Develop/rouster_ws/rouster/libs/foo/build") &&
              fs::exists(home / ".local/share/Trash/files/build") && fs::exists(home / "Develop/rouster_ws/rouster/src/main.cpp") &&
              fs::exists(home / "Develop/rouster_ws/fixes/patch.diff"),
              r.messages.empty() ? "" : r.messages.front());
        {
            core::scan::Progress pg;
            const dv::Found after = dv::find(H, pg);
            const dv::Project* a = nullptr;
            for (const auto& x : after.projects) if (x.rel == "Develop/rouster_ws") a = &x;
            check("dev: ...and the repo is still Backed after the clean", a && a->repo.status == dv::Status::Backed && a->builds.empty());
        }
        const auto u = door.undo();
        {
            const std::string now_s = files_snapshot(home);
            std::string diff;
            std::istringstream a(files_before), b(now_s);
            std::set<std::string> sa, sb;
            for (std::string l; std::getline(a, l);) sa.insert(l);
            for (std::string l; std::getline(b, l);) sb.insert(l);
            for (const auto& l : sa) if (!sb.count(l)) { diff = "-" + l; break; }
            if (diff.empty()) for (const auto& l : sb) if (!sa.count(l)) { diff = "+" + l; break; }
            check("dev: undo puts the build output back exactly", u.ok && now_s == files_before,
                  std::to_string(u.done) + " back " + diff);
        }

        // -- Retire / restore (s007b) ------------------------------------------
        {
            const auto outside_trash = [](const std::string& snap) {
                std::istringstream in(snap);
                std::string out;
                for (std::string l; std::getline(in, l);)
                    if (l.find("/.local/share/Trash/") == std::string::npos) out += l + "\n";
                return out;
            };
            core::scan::Progress pg;
            const dv::Found f1 = dv::find(H, pg);
            const dv::Project *rp = nullptr, *bp = nullptr;
            for (const auto& x : f1.projects) {
                if (x.rel == "Develop/rouster_ws") rp = &x;
                if (x.rel == "Develop/baker") bp = &x;
            }
            std::string why;
            check("retire: a workspace that is not clean is never retired",
                  bp && dv::retire_plan(H, *bp, 2000000000, why).actions.empty() && !why.empty(), why);
            dv::RetireView view;
            why.clear();
            dr::Plan rt = rp ? dv::retire_plan(H, *rp, 2000000000, why, &view) : dr::Plan{};
            const auto act = [&](std::size_t i) -> const dr::Action* { return i < rt.actions.size() ? &rt.actions[i] : nullptr; };
            const auto in_view = [](const std::vector<std::string>& v, const std::string& s) {
                return std::find(v.begin(), v.end(), s) != v.end();
            };
            check("retire: the plan -- write RESTORE.txt, Trash the app, backups/Old's items, all zips but the newest",
                  rt.actions.size() == 4 && act(0)->verb == dr::Verb::Write && act(0)->to == H + "/Develop/rouster_ws/RESTORE.txt" &&
                  act(1)->verb == dr::Verb::Trash && act(1)->from == H + "/Develop/rouster_ws/rouster" &&
                  act(2)->from == H + "/Develop/rouster_ws/backups/Old/s000_z.zip" &&
                  act(3)->from == H + "/Develop/rouster_ws/backups/s001_a.zip" && !rt.previewed && rt.label == "Retire rouster",
                  why.empty() ? std::to_string(rt.actions.size()) + " actions" : why);
            check("retire: keeps docs/, scripts/, fixes/, the newest zip; warns of the ignored file, not of build/",
                  in_view(view.keep, "docs/") && in_view(view.keep, "scripts/") && in_view(view.keep, "fixes/") &&
                  in_view(view.keep, "backups/s002_b.zip") && in_view(view.ignored, ".secrets.local") &&
                  !in_view(view.ignored, "build/") && !in_view(view.ignored, "libs/foo/build/"));
            dv::RestoreInfo ri;
            check("retire: RESTORE.txt reads back -- app, url, branch, the exact commit",
                  act(0) && dv::parse_restore(act(0)->content, ri) && ri.app == "rouster" && ri.branch == "main" &&
                  rp && ri.commit == rp->repo.head && ri.url == rp->repo.url && act(0)->content.find("git clone ") != std::string::npos);

            const std::string pre = outside_trash(files_snapshot(home));
            dr::Door door2(H, H + "/.local/share/rouster");
            door2.load();
            rt.previewed = true;
            const auto r1 = door2.run(rt);
            check("retire: run -- the app is gone, RESTORE.txt is there, docs / scripts / fixes / newest zip stay",
                  r1.ok && r1.done == 4 && !fs::exists(home / "Develop/rouster_ws/rouster") &&
                  fs::exists(home / "Develop/rouster_ws/RESTORE.txt") && fs::exists(home / "Develop/rouster_ws/docs/RULES.md") &&
                  fs::exists(home / "Develop/rouster_ws/fixes/patch.diff") && fs::exists(home / "Develop/rouster_ws/backups/s002_b.zip") &&
                  !fs::exists(home / "Develop/rouster_ws/backups/s001_a.zip") && fs::is_empty(home / "Develop/rouster_ws/backups/Old"),
                  r1.messages.empty() ? "" : r1.messages.front());
            core::scan::Progress pg2;
            const dv::Found f2 = dv::find(H, pg2);
            const dv::Project* gone = nullptr;
            for (const auto& x : f2.projects) if (x.rel == "Develop/rouster_ws") gone = &x;
            check("retire: find sees a Retired workspace, its RESTORE.txt read",
                  gone && gone->retired && gone->repo.status == dv::Status::Retired && gone->restore.commit == ri.commit &&
                  gone->name == "rouster" && gone->builds.empty() && gone->fixes_files == 1);
            why.clear();
            check("retire: a retired workspace is not retired twice", gone && dv::retire_plan(H, *gone, 0, why).actions.empty());

            why.clear();
            dr::Plan rs = gone ? dv::restore_plan(H, *gone, why) : dr::Plan{};
            check("restore: the plan -- Clone into rouster/ at the recorded branch + commit, Trash RESTORE.txt",
                  rs.actions.size() == 2 && rs.actions[0].verb == dr::Verb::Clone &&
                  rs.actions[0].to == H + "/Develop/rouster_ws/rouster" && rs.actions[0].commit == ri.commit &&
                  rs.actions[0].branch == "main" && rs.actions[1].from == H + "/Develop/rouster_ws/RESTORE.txt" &&
                  rs.label == "Restore rouster", why);
            rs.previewed = true;
            const auto r2 = door2.run(rs);
            const auto head = dv::git(H + "/Develop/rouster_ws/rouster", {"rev-parse", "HEAD"}, 10);
            check("restore: run -- the working copy is back at the commit, RESTORE.txt gone",
                  r2.ok && r2.done == 2 && head.out.rfind(ri.commit, 0) == 0 &&
                  fs::exists(home / "Develop/rouster_ws/rouster/src/main.cpp") && !fs::exists(home / "Develop/rouster_ws/RESTORE.txt"),
                  r2.messages.empty() ? head.out : r2.messages.front());
            core::scan::Progress pg3;
            const dv::Found f3 = dv::find(H, pg3);
            const dv::Project* back = nullptr;
            for (const auto& x : f3.projects) if (x.rel == "Develop/rouster_ws") back = &x;
            check("restore: and it is Backed up again", back && back->repo.status == dv::Status::Backed && !back->retired,
                  back ? std::string(dv::status_name(back->repo.status)) : "");
            const auto u2 = door2.undo();
            check("restore: undo -- the clone goes to the Trash, RESTORE.txt comes back",
                  u2.ok && !fs::exists(home / "Develop/rouster_ws/rouster") && fs::exists(home / "Develop/rouster_ws/RESTORE.txt"),
                  u2.messages.empty() ? "" : u2.messages.front());
            const auto u1 = door2.undo();
            check("retire: undo -- everything back exactly (outside the Trash)",
                  u1.ok && outside_trash(files_snapshot(home)) == pre, u1.messages.empty() ? "" : u1.messages.front());
        }
        // -- Lint: promote to known-optional (s007b) ---------------------------
        {
            const auto lb = dv::lint(H + "/Develop/baker", "baker", {"notes"});
            bool unknown_notes = false, promoted = false, unknown_stray = false;
            for (const auto& d : lb) {
                if (d.tier == dv::Tier::Unknown && d.name == "notes") unknown_notes = true;
                if (d.tier == dv::Tier::Unknown && d.name == "stray.txt") unknown_stray = true;
                if (d.tier == dv::Tier::Note && d.promoted && d.name == "notes") promoted = true;
            }
            check("lint: a promoted name is known-optional, shown as yours (forgettable); others stay unknown",
                  !unknown_notes && promoted && unknown_stray);
            const std::string kf = H + "/.local/share/rouster/known.json";
            check("known: save -> load round-trips, sorted and unique",
                  core::save_known(kf, {"notes", "art", "notes"}) && core::load_known(kf) == std::vector<std::string>{"art", "notes"});
            check("known: a missing file is an empty list", core::load_known(H + "/nope.json").empty());
        }

        core::scan::Progress pc;
        pc.cancel = true;
        check("dev: a cancelled find says so", dv::find(H, pc).cancelled && pc.done);
        core::scan::Progress pm;
        check("dev: refuses a missing home", !dv::find(H + "/nope", pm).refused.empty() && pm.done);

        fs::remove_all(home, ec);
        fs::remove_all(remotes, ec);
    }

    // -- Duplicates: find (read-only) -> keep rules -> plan -> the door (s008) --
    {
        std::cout << "\n-- duplicates --\n";
        namespace du = core::dups;
        namespace dr = core::door;
        const fs::path home = share_base() / "rouster_selftest_dups";
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::create_directories(home, ec);
        const std::string H = home.string();
        const bool can_share = du::can_share(H);
        std::cout << "  (fixture at " << H << (can_share ? ", btrfs: Share is real)\n" : ", not btrfs: Share must fail cleanly)\n");
        // keeper by place / by name
        put_bytes(home, "Documents/report.pdf", noise(20000, 1));
        put_bytes(home, "Downloads/report (1).pdf", noise(20000, 1));
        put_bytes(home, "Pictures/a.jpg", noise(50000, 2));
        put_bytes(home, "Pictures/a copy.jpg", noise(50000, 2));
        // too small to list
        put_bytes(home, "Documents/s1.txt", noise(100, 3));
        put_bytes(home, "Documents/s2.txt", noise(100, 3));
        // same size, same head and tail, different middle: only the whole hash tells
        std::string mid = noise(80000, 4);
        put_bytes(home, "Music/x.bin", mid);
        mid[40000] ^= 1;
        put_bytes(home, "Music/y.bin", mid);
        // hard links are one file
        put_bytes(home, "Videos/h1.bin", noise(30000, 5));
        fs::create_hard_link(home / "Videos/h1.bin", home / "Videos/h2.bin", ec);
        // a double-dumped backup: two whole folders, plus one file of it elsewhere
        for (const char* top : {"Backups/2024", "Backups/2024 (2)"}) {
            put_bytes(home, std::string(top) + "/p1.dat", noise(10000, 6));
            put_bytes(home, std::string(top) + "/sub/p2.dat", noise(8000, 7));
            put_bytes(home, std::string(top) + "/empty.txt", "");
        }
        put_bytes(home, "Documents/p1copy.dat", noise(10000, 6));
        // folders that differ only by a hidden file: not folder copies, the files still are
        put_bytes(home, "Old/a/q.dat", noise(9000, 8));
        put_bytes(home, "Old/b/q.dat", noise(9000, 8));
        put_bytes(home, "Old/b/.hidden", "x");
        // never seen: hidden, protected, build trees
        put_bytes(home, ".cache/d1.bin", noise(9000, 9));
        put_bytes(home, ".cache/d2.bin", noise(9000, 9));
        put_bytes(home, "Develop/w/fixes/f.bin", noise(9000, 10));
        put_bytes(home, "Develop/w/notes/f.bin", noise(9000, 10));
        put_bytes(home, "Develop/p/build/CMakeCache.txt", "x");
        put_bytes(home, "Develop/p/build/big.o", noise(9000, 11));
        put_bytes(home, "Develop/q/big.o", noise(9000, 11));
        fs::create_directories(home / ".local/share/Trash/files", ec);   // a home has a Trash already
        fs::create_directories(home / ".local/share/Trash/info", ec);
        fs::create_directories(home / ".local/share/rouster", ec);
        // keep rules, alone
        check("dups: copy_like -- 'x (2).pdf', 'Copy of x', 'x - Copy.txt', 'x_copy', 'x copy 3' yes; 'copyright.txt', 'x2.pdf' no",
              du::copy_like("x (2).pdf") && du::copy_like("Copy of x") && du::copy_like("x - Copy.txt") &&
              du::copy_like("x_copy") && du::copy_like("x copy 3") && !du::copy_like("copyright.txt") &&
              !du::copy_like("x2.pdf"));

        const std::string before = files_snapshot(home);
        du::Progress prog;
        du::Found f = du::find(H, H, prog);
        check("dups: find changes nothing", files_snapshot(home) == before && f.refused.empty() && prog.done);
        check("dups: reflink flag = the filesystem's", f.reflink == can_share);
        const auto find_group = [&](const std::string& rel) -> du::Group* {
            for (auto& g : f.groups)
                for (auto& m : g.members) if (m.rel == rel) return &g;
            return nullptr;
        };
        const auto keeper = [](const du::Group* g) { return g ? g->members[g->keep].rel : std::string("-"); };
        std::string listed;
        for (const auto& g : f.groups) {
            listed += g.folder ? "[D" : "[F";
            for (const auto& m : g.members) listed += " " + m.rel;
            listed += "] ";
        }
        check("dups: 5 groups -- 1 folder, 4 files", f.groups.size() == 5 && f.groups[0].folder, listed);
        check("dups: keeper not in Downloads", keeper(find_group("Downloads/report (1).pdf")) == "Documents/report.pdf" &&
              find_group("Documents/report.pdf")->keep_why == "not in Downloads");
        check("dups: keeper has the original name", keeper(find_group("Pictures/a copy.jpg")) == "Pictures/a.jpg" &&
              find_group("Pictures/a.jpg")->keep_why == "the original name");
        check("dups: under 4 KB not listed", !find_group("Documents/s1.txt"));
        check("dups: same head and tail, different middle -> not duplicates", !find_group("Music/x.bin"));
        check("dups: hard links are one file, not duplicates", !find_group("Videos/h1.bin"));
        {
            const du::Group* g = find_group("Backups/2024");
            check("dups: the double-dumped backup is ONE folder group, 3 files, keeper by name",
                  g && g->folder && g->members.size() == 2 && g->files == 3 && g->inner.size() == 3 &&
                  keeper(g) == "Backups/2024" && !find_group("Backups/2024/sub") && !find_group("Backups/2024 (2)/p1.dat"),
                  listed);
            const du::Group* p1 = find_group("Documents/p1copy.dat");
            check("dups: a file inside the folder copy isn't listed again; the keeper side's still is",
                  p1 && p1->members.size() == 2 && find_group("Backups/2024/p1.dat") == p1);
        }
        check("dups: a hidden file in a folder -> no folder group, its files still are",
              !find_group("Old/b") && find_group("Old/a/q.dat") && find_group("Old/a/q.dat") == find_group("Old/b/q.dat"));
        check("dups: never seen -- hidden, protected (fixes/), build trees",
              !find_group(".cache/d1.bin") && !find_group("Develop/w/notes/f.bin") && !find_group("Develop/q/big.o"));
        check("dups: hashing read only what it had to", f.hashed > 0 && f.hashed < 2 * 400000, std::to_string(f.hashed));

        // Trash mode: nothing pre-checked (only when chosen).
        du::preset(f, du::Mode::Trash);
        std::vector<std::string> dropped;
        check("dups: Trash mode pre-checks nothing -> an empty plan",
              du::plan(H, f, du::Mode::Trash, dropped).actions.empty());
        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        {
            du::Group* g = find_group("Downloads/report (1).pdf");
            for (auto& m : g->members) m.checked = m.rel == "Downloads/report (1).pdf";
            dropped.clear();
            dr::Plan p = du::plan(H, f, du::Mode::Trash, dropped);
            check("dups: one copy checked -> one Trash action; the door refuses it unpreviewed",
                  p.actions.size() == 1 && p.actions[0].verb == dr::Verb::Trash && !door.run(p).ok);
            p.previewed = true;
            const auto r = door.run(p);
            check("dups: run -> the copy in the Trash, the keeper stays",
                  r.ok && !fs::exists(home / "Downloads/report (1).pdf") && fs::exists(home / "Documents/report.pdf") &&
                  fs::exists(home / ".local/share/Trash/files/report (1).pdf"));
            const auto u = door.undo();
            check("dups: undo -> exact", u.ok && files_snapshot(home) == before);
            for (auto& m : g->members) m.checked = false;
        }
        {   // the keeper never goes with its copies
            du::Group* fg = find_group("Backups/2024");
            fg->keep = 1;                                   // keep "2024 (2)": "2024" is now the copy
            fg->members[0].checked = true;
            du::Group* pg = find_group("Documents/p1copy.dat");   // keeper Backups/2024/p1.dat -- inside the trashed copy
            for (auto& m : pg->members) m.checked = m.rel == "Documents/p1copy.dat";
            dropped.clear();
            const dr::Plan p = du::plan(H, f, du::Mode::Trash, dropped);
            const bool only_folder = p.actions.size() == 1 && p.actions[0].from == H + "/Backups/2024";
            check("dups: a keeper inside a trashed folder -> its group left out, said why",
                  only_folder && dropped.size() == 1 && dropped[0].find("would go to the Trash") != std::string::npos,
                  dropped.empty() ? "" : dropped[0]);
            fg->keep = 0; fg->members[0].checked = false;
            for (auto& m : pg->members) m.checked = false;
        }
        {   // changed since Look
            du::Group* g = find_group("Pictures/a copy.jpg");
            for (auto& m : g->members) m.checked = m.rel == "Pictures/a copy.jpg";
            struct timespec ts[2] = {{1000000000, 0}, {1000000000, 0}};
            ::utimensat(AT_FDCWD, (home / "Pictures/a copy.jpg").c_str(), ts, 0);
            dropped.clear();
            const dr::Plan p = du::plan(H, f, du::Mode::Trash, dropped);
            check("dups: a copy changed since Look -> left out, said why",
                  p.actions.empty() && dropped.size() == 1 && dropped[0].find("changed since Look") != std::string::npos);
            for (auto& m : g->members) m.checked = false;
        }
        // Fresh look for Share (the mtime above moved).
        du::Progress prog2;
        f = du::find(H, H, prog2);
        const std::string before2 = files_snapshot(home);
        du::preset(f, du::Mode::Share);
        dropped.clear();
        dr::Plan sp = du::plan(H, f, du::Mode::Share, dropped);
        std::size_t inner_acts = 0;
        for (const auto& a : sp.actions) if (a.to.find("/Backups/2024 (2)/") != std::string::npos) ++inner_acts;
        check("dups: Share mode -> a Dedupe per copy; the folder's files >= 4 KB each (empty.txt not)",
              sp.actions.size() == 6 && inner_acts == 2 &&
              std::all_of(sp.actions.begin(), sp.actions.end(), [](auto& a) { return a.verb == dr::Verb::Dedupe; }),
              std::to_string(sp.actions.size()));
        sp.previewed = true;
        const auto r = door.run(sp);
        if (can_share) {
            check("dups: share run -> every pair shares its blocks", r.ok && r.done == sp.actions.size() &&
                  std::all_of(sp.actions.begin(), sp.actions.end(), [](auto& a) { return du::same_blocks(a.from, a.to); }),
                  r.messages.empty() ? "" : r.messages.front());
            check("dups: ...and every path, size, mtime, inode is as it was", files_snapshot(home) == before2);
            du::Progress prog3;
            du::Found again = du::find(H, H, prog3);
            std::uint64_t gain = 0;
            for (const auto& g : again.groups) gain += g.gain(du::Mode::Share);
            check("dups: Look again -> already shared, nothing pre-checked, nothing to gain", gain == 0);
        } else {
            check("dups: share run (not btrfs) -> fails cleanly, tree exact",
                  !r.ok && r.done == 0 && files_snapshot(home) == before2, r.messages.empty() ? "" : r.messages.front());
        }
        const auto u = door.undo();
        check("dups: undo the share -> exact", files_snapshot(home) == before2, u.messages.empty() ? "" : u.messages.front());
        {   // door policy for Dedupe
            dr::Action hl; hl.verb = dr::Verb::Dedupe; hl.from = H + "/Videos/h1.bin"; hl.to = H + "/Videos/h2.bin";
            dr::Action sz; sz.verb = dr::Verb::Dedupe; sz.from = H + "/Pictures/a.jpg"; sz.to = H + "/Documents/report.pdf";
            dr::Action pr; pr.verb = dr::Verb::Dedupe; pr.from = H + "/Develop/w/notes/f.bin"; pr.to = H + "/Develop/w/fixes/f.bin";
            dr::Action dir; dir.verb = dr::Verb::Dedupe; dir.from = H + "/Old/a"; dir.to = H + "/Old/b";
            dr::Action out; out.verb = dr::Verb::Dedupe; out.from = H + "/Pictures/a.jpg"; out.to = "/etc/hostname";
            check("door: dedupe refuses -- a hard link, different sizes, protected, a folder, outside home",
                  !door.refusal(hl).empty() && !door.refusal(sz).empty() && !door.refusal(pr).empty() &&
                  !door.refusal(dir).empty() && !door.refusal(out).empty(), door.refusal(pr));
        }
        if (can_share) {   // the kernel compares: a same-size file that differs is never shared
            const std::string b3 = files_snapshot(home);
            dr::Action d; d.verb = dr::Verb::Dedupe; d.from = H + "/Music/x.bin"; d.to = H + "/Music/y.bin";
            const auto rr = door.run(dr::Plan{"differ", {d}, true});
            check("door: dedupe of different contents -> failed, says so, nothing changed",
                  !rr.ok && rr.failed == 1 && !rr.messages.empty() && rr.messages[0].find("differ") != std::string::npos &&
                  files_snapshot(home) == b3 && !du::same_blocks(d.from, d.to), rr.messages.empty() ? "" : rr.messages[0]);
            door.undo();
        }
        {
            du::Progress pc;
            pc.cancel = true;
            const du::Found c = du::find(H, H, pc);
            check("dups: cancel -> stops, says so", c.cancelled && c.groups.empty() && pc.done);
            du::Progress po;
            check("dups: refuses outside home and protected roots",
                  !du::find(H, "/etc", po).refused.empty() && !du::find(H, H + "/Develop/w/fixes", po).refused.empty());
        }
        fs::remove_all(home, ec);
    }

    // -- Archive: find (read-only) -> plan (MakeDir + Move) -> door -> put back (s009)
    {
        std::cout << "\n-- archive --\n";
        namespace ar = core::archive;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_archive";
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::create_directories(home / ".local/share/Trash/files", ec);
        fs::create_directories(home / ".local/share/Trash/info", ec);
        fs::create_directories(home / ".local/share/rouster", ec);
        const std::string H = home.string();
        const std::int64_t now = 2000000000, old = 1830000000;   // 2033 / end of 2027
        const std::vector<std::string> olds = {"Documents/old.pdf", "Documents/trip/a.jpg", "Documents/trip/b.jpg",
            "Documents/trip/sub/c.txt", "Documents/mixed/o.txt", "Documents/hid/x.txt", "Documents/hid/.h",
            "Pictures/p.png", "notes.txt", "Develop/proj/old.c", "Music/repo/song.wav", ".cache/oldc",
            "Archive/2020/x.txt", "Documents/fixes/f.txt"};
        for (const auto& r : olds) put(home, r, 5000);
        put(home, "Documents/new.pdf", 5000);
        put(home, "Documents/mixed/n.txt", 5000);
        fs::create_directories(home / "Music/repo/.git", ec);
        for (const char* r : {"Documents/new.pdf", "Documents/mixed/n.txt"}) {   // new by the pinned clock
            struct timespec ts[2] = {{now - 86400, 0}, {now - 86400, 0}};
            ::utimensat(AT_FDCWD, (home / r).c_str(), ts, AT_SYMLINK_NOFOLLOW);
        }
        for (const auto& r : olds) {
            struct timespec ts[2] = {{old, 0}, {old, 0}};
            ::utimensat(AT_FDCWD, (home / r).c_str(), ts, AT_SYMLINK_NOFOLLOW);
        }
        std::tm tm{};
        std::time_t ot = old;
        ::localtime_r(&ot, &tm);
        const int Y = tm.tm_year + 1900;
        const std::string YS = std::to_string(Y);

        check("archive: destination", ar::destination("Documents/x.pdf", 2019) == "Archive/2019/Documents/x.pdf");
        const std::string before = files_snapshot(home);
        core::scan::Progress prog;
        ar::Found f = ar::find(H, now, 12, prog);
        check("archive: find changes nothing", files_snapshot(home) == before && f.refused.empty() && prog.done);
        std::string got;
        for (const auto& it : f.items) got += it.rel + (it.folder ? "/ " : " ");
        const std::set<std::string> want = {"Documents/hid/x.txt", "Documents/mixed/o.txt", "Documents/old.pdf",
                                            "Documents/trip", "Pictures/p.png", "notes.txt"};
        std::set<std::string> have;
        for (const auto& it : f.items) have.insert(it.rel);
        check("archive: exactly the old things -- a whole old folder as one, a mixed folder's old file, top files",
              have == want, got);
        const auto item = [&](const std::string& rel) -> ar::Item* {
            for (auto& it : f.items) if (it.rel == rel) return &it;
            return nullptr;
        };
        check("archive: the old folder is one item of 3 files, its year the newest file's",
              item("Documents/trip") && item("Documents/trip")->folder && item("Documents/trip")->files == 3 &&
              item("Documents/trip")->year == Y);
        check("archive: never seen -- new files, dev roots, repos, hidden, ~/Archive, fixes/",
              !item("Documents/new.pdf") && !item("Develop/proj/old.c") && !item("Music/repo/song.wav") &&
              !item(".cache/oldc") && !item("Archive/2020/x.txt") && !item("Documents/fixes/f.txt") && !item("Documents/hid"));
        check("archive: nothing pre-checked",
              std::none_of(f.items.begin(), f.items.end(), [](auto& it) { return it.checked; }));

        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        item("Documents/trip")->checked = true;
        item("Documents/old.pdf")->checked = true;
        std::vector<std::string> dropped;
        dr::Plan p = ar::plan(H, f.items, dropped);
        std::size_t mk = 0, mv = 0;
        for (const auto& a : p.actions) (a.verb == dr::Verb::MakeDir ? mk : mv)++;
        check("archive: plan -- 2 folders made (~/Archive is there: <year>, <year>/Documents, parents first), then 2 moves",
              mk == 2 && mv == 2 && p.actions[0].to == H + "/Archive/" + YS && p.actions[1].to == H + "/Archive/" + YS + "/Documents" &&
              dropped.empty(), std::to_string(mk) + "+" + std::to_string(mv));
        check("archive: the door refuses it unpreviewed", !door.run(p).ok && files_snapshot(home) == before);
        p.previewed = true;
        auto r = door.run(p);
        check("archive: run -> in ~/Archive/<year>/<path>, gone from where they were",
              r.ok && fs::exists(home / ("Archive/" + YS + "/Documents/trip/sub/c.txt")) &&
              fs::exists(home / ("Archive/" + YS + "/Documents/old.pdf")) && !fs::exists(home / "Documents/trip") &&
              !fs::exists(home / "Documents/old.pdf"), r.messages.empty() ? "" : r.messages.front());
        const auto outside_trash = [](const std::string& snap) {
            std::istringstream in(snap);
            std::string o;
            for (std::string l; std::getline(in, l);)
                if (l.find("/.local/share/Trash/") == std::string::npos) o += l + "\n";
            return o;
        };
        auto u = door.undo();
        check("archive: undo -> exact (the made folders, empty again, went to the Trash)",
              u.ok && outside_trash(files_snapshot(home)) == outside_trash(before), u.messages.empty() ? "" : u.messages.front());

        // Again, then put back from the manifest -- not through Undo.
        r = door.run(p);
        std::vector<ar::Stored> st = ar::stored(door, H);
        check("archive: the manifest lists what is in the archive, and where it came from",
              r.ok && st.size() == 2 && st[0].back == "Documents/old.pdf" && st[1].back == "Documents/trip" && st[1].folder);
        put(home, "Documents/old.pdf", 10);   // something new where one came from
        st = ar::stored(door, H);
        check("archive: a place that's taken now is blocked", st[0].blocked && !st[1].blocked);
        for (auto& s : st) s.checked = true;
        dropped.clear();
        dr::Plan pb = ar::put_back(H, st, dropped);
        check("archive: put back leaves the blocked one, says why",
              pb.actions.size() == 1 && pb.actions[0].to == H + "/Documents/trip" && dropped.size() == 1);
        pb.previewed = true;
        r = door.run(pb);
        check("archive: put back -> home again; the archive keeps the rest",
              r.ok && fs::exists(home / "Documents/trip/sub/c.txt") && ar::stored(door, H).size() == 1);
        fs::remove(home / "Documents/old.pdf", ec);
        {   // a put back whose folder is gone makes it again
            fs::remove_all(home / "Documents/mixed", ec);
            fs::remove_all(home / "Documents/hid", ec);
            fs::remove_all(home / "Documents/new.pdf", ec);
            fs::rename(home / "Documents/trip", home / "Pictures/trip", ec);   // Documents now empty...
            fs::remove(home / "Documents/fixes/f.txt", ec);
            fs::remove(home / "Documents/fixes", ec);
            fs::remove(home / "Documents", ec);                                // ...and gone
            auto s2 = ar::stored(door, H);
            for (auto& s : s2) s.checked = true;
            dropped.clear();
            dr::Plan p2 = ar::put_back(H, s2, dropped);
            p2.previewed = true;
            const auto r2 = door.run(p2);
            check("archive: put back into a folder that's gone -> MakeDir, then the move",
                  p2.actions.size() == 2 && p2.actions[0].verb == dr::Verb::MakeDir && r2.ok &&
                  fs::exists(home / "Documents/old.pdf"), r2.messages.empty() ? "" : r2.messages.front());
        }
        {   // the made folder isn't trashed by an undo while something is in it
            core::scan::Progress p3;
            ar::Found f3 = ar::find(H, now, 12, p3);
            for (auto& it : f3.items) it.checked = it.rel == "notes.txt";
            dropped.clear();
            dr::Plan p4 = ar::plan(H, f3.items, dropped);
            p4.previewed = true;
            door.run(p4);   // makes Archive/<Y>/... no: Archive/<Y> exists; notes.txt goes straight in
            dr::Action mk1; mk1.verb = dr::Verb::MakeDir; mk1.to = H + "/Music/box";
            door.run(dr::Plan{"box", {mk1}, true});
            put(home, "Music/box/mine.txt", 10);
            const auto ub = door.undo();
            check("archive: undo of a made folder that has something in it -> left in place, said so",
                  !ub.ok && fs::exists(home / "Music/box/mine.txt") && !ub.messages.empty() &&
                  ub.messages[0].find("left in place") != std::string::npos, ub.messages.empty() ? "" : ub.messages[0]);
        }
        {   // the door's plan-order policy
            dr::Action m1; m1.verb = dr::Verb::MakeDir; m1.to = H + "/Archive/x";
            dr::Action m2; m2.verb = dr::Verb::MakeDir; m2.to = H + "/Archive/x/y";
            dr::Action mvv; mvv.verb = dr::Verb::Move; mvv.from = H + "/notes2.txt"; mvv.to = H + "/Archive/x/y/notes2.txt";
            put(home, "notes2.txt", 10);
            check("door: a Move into a folder an earlier MakeDir makes -- allowed; without it -- refused",
                  door.refusal(mvv, {m1.to, m2.to}).empty() && !door.refusal(mvv).empty());
            dr::Action pr; pr.verb = dr::Verb::MakeDir; pr.to = H + "/.ssh/x";
            const auto twice = door.run(dr::Plan{"twice", {m1, m1}, true});
            check("door: mkdir refuses -- the same folder twice in a plan, protected, an existing one",
                  !twice.ok && twice.done == 0 && !door.refusal(pr).empty() &&
                  !door.refusal(dr::Action{dr::Verb::MakeDir, "", H + "/Music"}).empty());
        }
        {
            core::scan::Progress pc;
            pc.cancel = true;
            check("archive: cancel -> stops, says so", ar::find(H, now, 12, pc).cancelled);
        }
        fs::remove_all(home, ec);
    }

    // -- Organize: templates, new project, file into project, new version (s010)
    {
        std::cout << "\n-- organize --\n";
        namespace og = core::organize;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_organize";
        std::error_code ec;
        fs::remove_all(home, ec);
        for (const char* d : {".local/share/Trash/files", ".local/share/Trash/info", ".local/share/rouster", ".config",
                              "Documents", "Downloads"})
            fs::create_directories(home / d, ec);
        const std::string H = home.string();
        const auto there = [](const fs::path& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0; };
        const auto slurp = [](const fs::path& p) { std::ifstream in(p, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); };
        const auto outside_trash = [](const std::string& snap) {
            std::istringstream in(snap);
            std::string out;
            for (std::string l; std::getline(in, l);)
                if (l.find("/.local/share/Trash/") == std::string::npos) out += l + "\n";
            return out;
        };
        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        const auto go = [&](dr::Plan p) { p.previewed = true; return door.run(p); };

        check("organize: slug -- spaces and _ / become one '-', never hidden",
              og::slug("Spring Catalog_2026 / v2") == "Spring-Catalog-2026-v2" && og::slug(".hidden  x") == "hidden-x" &&
              og::slug("a - b") == "a-b", og::slug("Spring Catalog_2026 / v2"));
        const std::int64_t T0 = 1793000000;   // pinned clock for names
        const og::Values v{"2611", "Acme Spring Catalog", T0};
        check("organize: fill -- names slugged, text as typed",
              og::fill("{id}_{title}_{yyyymmdd}", v) == "2611_Acme-Spring-Catalog_" + og::stamp(T0, false) &&
              og::fill("# {title}", v, false) == "# Acme Spring Catalog");
        check("organize: no templates yet", og::templates(H).empty());

        // Starters: MakeDir + Write through the door, one step, undoable.
        std::vector<std::string> skipped;
        const std::string b0 = files_snapshot(home);
        auto sp = og::starters(H, skipped);
        check("organize: starters plan changes nothing; three templates, nothing skipped",
              files_snapshot(home) == b0 && skipped.empty() && !sp.actions.empty());
        auto r = go(sp);
        check("organize: starters through the door", r.ok, r.messages.empty() ? "" : r.messages.front());
        auto ts = og::templates(H);
        check("organize: three templates, each readable",
              ts.size() == 3 && ts[0].name == "Book" && ts[1].name == "Design job" && ts[2].name == "gtkmm app" &&
              ts[0].problem.empty() && ts[1].problem.empty() && ts[2].problem.empty() &&
              ts[1].rules.pattern == "{id}_{title}_{yyyymmdd-hhmm}_{part}.{ext}" && ts[1].root == "Documents/Jobs");
        {
            std::vector<std::string> sk;
            check("organize: starters again -- all three skipped, nothing to do", og::starters(H, sk).actions.empty() && sk.size() == 3);
        }
        {
            const auto u = door.undo();
            check("organize: undo the starters -- exact outside the Trash", u.ok && outside_trash(files_snapshot(home)) == outside_trash(b0),
                  u.messages.empty() ? "" : u.messages.front());
            std::vector<std::string> sk;
            go(og::starters(H, sk));
            check("organize: starters again after undo", og::templates(H).size() == 3);
        }
        ts = og::templates(H);
        og::Template design = ts[1];
        // A binary file in the template is copied, not written.
        const std::string blank = std::string("\0\x01PDF-ish\0", 10) + noise(30000, 9);
        put_bytes(fs::path(design.path), "finals/{id}_blank.bin", blank);

        // New project.
        std::vector<std::string> probs;
        const std::string b2 = files_snapshot(home);
        auto np = og::new_project(H, design, v, probs);
        const std::string pdir = H + "/Documents/Jobs/2611_Acme-Spring-Catalog";
        std::size_t copies = 0, writes = 0;
        for (const auto& a : np.actions) { copies += a.verb == dr::Verb::Copy; writes += a.verb == dr::Verb::Write; }
        check("organize: new project plan changes nothing; makes Jobs/ first; one Copy, two Writes",
              files_snapshot(home) == b2 && probs.empty() && og::project_dir(H, design, v) == pdir &&
              np.actions.front().verb == dr::Verb::MakeDir && np.actions.front().to == H + "/Documents/Jobs" &&
              copies == 1 && writes == 2, probs.empty() ? "" : probs.front());
        r = go(np);
        const std::string S0 = og::stamp(T0);
        const fs::path brief = fs::path(pdir) / ("brief/2611_Acme-Spring-Catalog_" + S0 + "_brief.md");
        check("organize: new project through the door -- the tree, placeholders filled",
              r.ok && fs::is_directory(pdir + "/workups") && fs::is_directory(pdir + "/versions") &&
              slurp(brief).rfind("# Acme Spring Catalog\n\nJob 2611", 0) == 0,
              r.messages.empty() ? brief.string() : r.messages.front());
        check("organize: the binary template file copied byte for byte", slurp(pdir + "/finals/2611_blank.bin") == blank);
        og::Project pj;
        check("organize: the project carries its own rules",
              og::load_project(pdir, pj) && pj.id == "2611" && pj.title == "Acme Spring Catalog" &&
              pj.tmpl == "Design job" && pj.rules.pattern == design.rules.pattern && og::route(pj, "OTF") == "fonts");
        check("organize: projects() finds it", og::projects(H, og::templates(H)).size() == 1);
        {
            std::vector<std::string> p2, p3;
            og::new_project(H, design, v, p2);
            og::new_project(H, design, og::Values{"", "x", T0}, p3);
            check("organize: refused -- already there; no id", !p2.empty() && !p3.empty());
        }

        // Naming.
        og::Parts parts;
        check("organize: fits -- the brief fits, a stray doesn't",
              og::fits(pj, brief.filename().string(), &parts) && parts.part == "brief" && parts.ext == "md" && parts.stamp == S0 &&
              !og::fits(pj, "stray notes.txt") && !og::fits(pj, "2612_Acme-Spring-Catalog_" + S0 + "_x.md"));

        // File into project.
        for (const char* f : {"Downloads/Acme logo FINAL.ai", "Downloads/Brandon.otf", "Downloads/notes", "Downloads/a/x.ai",
                              "Downloads/b/x.ai"})
            put(home, f, 6000);
        fs::create_directories(home / "Downloads/folder", ec);
        for (const char* f : {"Downloads/a/x.ai", "Downloads/b/x.ai"}) {   // same name, same minute -> -2
            struct timespec ts2[2] = {{T0, 0}, {T0, 0}};
            ::utimensat(AT_FDCWD, (home / f).c_str(), ts2, AT_SYMLINK_NOFOLLOW);
        }
        const std::string b3 = files_snapshot(home);
        auto in = og::propose(H, pj, {H + "/Downloads/Acme logo FINAL.ai", H + "/Downloads/Brandon.otf", H + "/Downloads/notes",
                                      H + "/Downloads/a/x.ai", H + "/Downloads/b/x.ai", H + "/Downloads/folder", "/etc/hostname",
                                      brief.string()});
        const auto stamp_of = [&](const char* rel) { struct stat st{}; ::lstat((home / rel).c_str(), &st); return og::stamp(st.st_mtim.tv_sec); };
        const std::string P = pdir + "/";
        const std::string N = "2611_Acme-Spring-Catalog_";
        check("organize: propose changes nothing", files_snapshot(home) == b3 && in.size() == 8);
        check("organize: named by the pattern, routed by type",
              in[0].to == P + "workups/" + N + stamp_of("Downloads/Acme logo FINAL.ai") + "_Acme-logo-FINAL.ai" &&
              in[1].to == P + "fonts/" + N + stamp_of("Downloads/Brandon.otf") + "_Brandon.otf" &&
              in[2].to == P + N + stamp_of("Downloads/notes") + "_notes", in[0].to + " | " + in[2].to);
        check("organize: the same name twice -> the second gets -2",
              in[3].to == P + "workups/" + N + S0 + "_x.ai" && in[4].to == P + "workups/" + N + S0 + "_x-2.ai", in[4].to);
        check("organize: not filed -- a folder, outside home, already filed",
              in[5].to.empty() && !in[5].checked && in[6].to.empty() && in[7].to.empty() && in[7].note == "already filed");
        in[2].checked = false;   // unchecked stays
        {   // changed since propose -> left out
            struct timespec ts3[2] = {{T0 + 99, 0}, {T0 + 99, 0}};
            ::utimensat(AT_FDCWD, (home / "Downloads/b/x.ai").c_str(), ts3, AT_SYMLINK_NOFOLLOW);
        }
        std::vector<std::string> dropped;
        auto fp = og::file_plan(pj, in, dropped);
        check("organize: file plan -- 3 moves, the changed one left out, unchecked stays",
              fp.actions.size() == 3 && dropped.size() == 1 && dropped[0].find("changed since") != std::string::npos);
        const std::string b4 = files_snapshot(home);
        r = go(fp);
        check("organize: filed through the door",
              r.ok && there(in[0].to) && there(in[1].to) && there(in[3].to) && !there(home / "Downloads/Brandon.otf") &&
              there(home / "Downloads/notes") && there(home / "Downloads/b/x.ai"), r.messages.empty() ? "" : r.messages.front());
        auto u = door.undo();
        check("organize: undo the filing -- exact", u.ok && files_snapshot(home) == b4);
        {   // a route to a folder the project doesn't have yet -> MakeDir first
            og::Project q = pj;
            q.rules.routes.push_back({"sketches/raw", {"zzz"}});
            put(home, "Downloads/s.zzz", 3000);
            std::vector<std::string> d2;
            auto qp = og::file_plan(q, og::propose(H, q, {H + "/Downloads/s.zzz"}), d2);
            check("organize: a missing route folder is made first (parents first), then the move",
                  qp.actions.size() == 3 && qp.actions[0].verb == dr::Verb::MakeDir && qp.actions[0].to == P + "sketches" &&
                  qp.actions[1].to == P + "sketches/raw" && qp.actions[2].verb == dr::Verb::Move);
            const std::string b5 = files_snapshot(home);
            r = go(qp);
            u = door.undo();
            check("organize: ...through the door and back exactly (outside the Trash)", r.ok && u.ok && outside_trash(files_snapshot(home)) == outside_trash(b5), u.messages.empty() ? "" : u.messages.front());
        }

        // Flagged, not moved.
        put(home, "Documents/Jobs/2611_Acme-Spring-Catalog/stray notes.txt", 100);
        {
            const auto c = og::contents(pj);
            std::size_t misfit = 0;
            bool hidden = false;
            for (const auto& e : c) { misfit += !e.fits; hidden |= e.rel.find(".rouster") != std::string::npos; }
            check("organize: contents -- the stray and the template's blank flagged; .rouster.json not listed",
                  misfit == 2 && !hidden, std::to_string(misfit));
        }

        // New version.
        const std::string rel = "brief/" + brief.filename().string();
        const std::int64_t T1 = T0 + 3600;
        std::string why;
        auto vp = og::new_version(pj, rel, T1, why);
        const std::string fresh = P + "brief/" + N + og::stamp(T1) + "_brief.md";
        check("organize: new version plan -- Copy to the new stamp, then the old into versions/",
              why.empty() && vp.actions.size() == 2 && vp.actions[0].verb == dr::Verb::Copy && vp.actions[0].to == fresh &&
              vp.actions[1].verb == dr::Verb::Move && vp.actions[1].to == P + "versions/" + brief.filename().string(), why);
        const std::string body = slurp(brief);
        const std::string b6 = files_snapshot(home);
        r = go(vp);
        check("organize: new version through the door -- same text, old one in versions/",
              r.ok && slurp(fresh) == body && !there(brief) && there(P + "versions/" + brief.filename().string()),
              r.messages.empty() ? "" : r.messages.front());
        {
            std::string w1, w2, w3;
            og::new_version(pj, "brief/" + fs::path(fresh).filename().string(), T1, w1);
            og::new_version(pj, "stray notes.txt", T1, w2);
            og::new_version(pj, "versions/" + brief.filename().string(), T1 + 600, w3);
            check("organize: no new version -- this minute again, a misfit, an old version",
                  !w1.empty() && !w2.empty() && !w3.empty(), w1);
        }
        u = door.undo();
        check("organize: undo the new version -- exact outside the Trash, the copy in the Trash",
              u.ok && outside_trash(files_snapshot(home)) == outside_trash(b6) &&
              there(home / (".local/share/Trash/files/" + fs::path(fresh).filename().string())),
              u.messages.empty() ? "" : u.messages.front());
        {   // Copy refusals
            fs::create_directories(home / ".ssh", ec);
            put(home, ".ssh/key", 10);
            dr::Action c1; c1.verb = dr::Verb::Copy; c1.from = H + "/Documents"; c1.to = H + "/Documents/d2";
            dr::Action c2; c2.verb = dr::Verb::Copy; c2.from = H + "/.ssh/key"; c2.to = H + "/Documents/key";
            dr::Action c3; c3.verb = dr::Verb::Copy; c3.from = "/etc/hostname"; c3.to = H + "/Documents/h";
            dr::Action c4; c4.verb = dr::Verb::Copy; c4.from = brief.string(); c4.to = pdir + "/stray notes.txt";
            dr::Action c5; c5.verb = dr::Verb::Copy; c5.from = brief.string(); c5.to = H + "/.ssh/b.md";
            check("door: copy refuses -- a folder, a protected source, outside home, an existing target, a protected target",
                  !door.refusal(c1).empty() && !door.refusal(c2).empty() && !door.refusal(c3).empty() &&
                  !door.refusal(c4).empty() && !door.refusal(c5).empty());
        }
        {   // the manifest keeps the copy's source
            dr::Door d2(H, H + "/.local/share/rouster");
            d2.load();
            bool src = false;
            for (const auto& b : d2.batches())
                for (auto id : b.records)
                    if (const auto* rec = d2.record(id); rec && rec->verb == dr::Verb::Copy && rec->src == brief.string()) src = true;
            check("door: the manifest records what a copy was made from", src);
        }
        fs::remove_all(home, ec);
    }

    // -- Rules: match (read-only) -> plan -> door (s011) ---------------------
    {
        std::cout << "\n-- rules --\n";
        namespace rl = core::rules;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_rules";
        std::error_code ec;
        fs::remove_all(home, ec);
        for (const char* d : {".local/share/Trash/files", ".local/share/Trash/info", ".local/share/rouster", "Downloads", "Documents"})
            fs::create_directories(home / d, ec);
        const std::string H = home.string();
        const std::string data = H + "/.local/share/rouster";
        const std::int64_t now = 2000000000;
        const auto age = [&](const char* rel, std::int64_t t) {
            struct timespec ts[2] = {{t, 0}, {t, 0}};
            ::utimensat(AT_FDCWD, (home / rel).c_str(), ts, AT_SYMLINK_NOFOLLOW);
        };
        const std::int64_t D = 86400;
        for (const char* f : {"Downloads/Invoice-March.PDF", "Downloads/manual.pdf", "Downloads/setup.iso", "Downloads/new.iso",
                              "Downloads/movie.mp4.part", "Downloads/.hidden.pdf", "Downloads/sub/deep.pdf", "Downloads/Font.otf",
                              "Downloads/arriving.pdf", "Downloads/old.zip"})
            put(home, f, 4000);
        age("Downloads/Invoice-March.PDF", now - 10 * D); age("Downloads/manual.pdf", now - 3 * D);
        age("Downloads/setup.iso", now - 60 * D);         age("Downloads/new.iso", now - 2 * D);
        age("Downloads/movie.mp4.part", now - 90 * D);    age("Downloads/.hidden.pdf", now - 90 * D);
        age("Downloads/sub/deep.pdf", now - 90 * D);      age("Downloads/Font.otf", now - 1 * D);
        age("Downloads/arriving.pdf", now - 30);          age("Downloads/old.zip", now - 400 * D);

        check("rules: glob -- case-insensitive * and ?",
              rl::glob_match("*invoice*", "Invoice-March.PDF") && rl::glob_match("Screenshot*", "screenshot 1.png") &&
              rl::glob_match("a?c", "abc") && !rl::glob_match("*.pdf", "x.pdfx") && rl::glob_match("*", ""));
        check("rules: partial downloads are never touched",
              rl::in_progress("movie.mp4.part") && rl::in_progress("x.CRDOWNLOAD") && !rl::in_progress("x.pdf"));

        rl::Book book;
        rl::Rule inv; inv.name = "Invoices"; inv.glob = "*invoice*"; inv.act = rl::Do::Move; inv.dest = "Documents/Invoices/{year}";
        inv.rename = "{yyyymmdd}_{stem}.{ext}"; inv.enabled = true;
        rl::Rule pdf; pdf.name = "PDFs"; pdf.exts = {"pdf"}; pdf.older_days = 7; pdf.act = rl::Do::Move; pdf.dest = "Documents/PDFs"; pdf.enabled = true;
        rl::Rule iso; iso.name = "Installers"; iso.exts = {"iso"}; iso.older_days = 30; iso.act = rl::Do::Trash; iso.enabled = true;
        rl::Rule arc; arc.name = "Old"; arc.older_days = 180; arc.act = rl::Do::Archive; arc.enabled = true;
        rl::Rule off; off.name = "Fonts"; off.exts = {"otf"}; off.act = rl::Do::Move; off.dest = "Documents/Fonts";   // not enabled
        book.rules = {inv, pdf, iso, arc, off};
        book.watch = true;
        {
            const auto b2 = rl::from_json(rl::to_json(book));
            check("rules: the book round-trips through JSON",
                  b2.watch && b2.rules.size() == 5 && b2.rules[0].rename == inv.rename && b2.rules[2].act == rl::Do::Trash &&
                  b2.rules[1].exts == pdf.exts && !b2.rules[4].enabled && rl::from_json("not json").rules.empty());
            check("rules: saved in Rouster's own folder, read back",
                  core::save_own(data, rl::kFile, rl::to_json(book)) && rl::load(data + "/" + rl::kFile).rules.size() == 5 &&
                  !core::save_own(data, "../x.json", "{}"));
        }
        rl::Rule all; all.name = "All"; all.act = rl::Do::Trash;
        rl::Rule self; self.name = "Self"; self.exts = {"pdf"}; self.dest = "Downloads";
        rl::Rule nofolder = pdf; nofolder.folder = "Nope";
        rl::Rule nofile = pdf; nofile.act = rl::Do::File; nofile.dest = "Documents";
        check("rules: problems -- matches everything, into itself, no folder, not a project; a good rule has none",
              !rl::problem(all, H).empty() && !rl::problem(self, H).empty() && !rl::problem(nofolder, H).empty() &&
              !rl::problem(nofile, H).empty() && rl::problem(pdf, H).empty() && rl::problem(arc, H).empty());

        const std::string before = files_snapshot(home);
        auto ms = rl::matches(H, book.rules, now);
        check("rules: matching changes nothing", files_snapshot(home) == before);
        std::map<std::string, std::pair<std::size_t, std::string>> got;
        for (const auto& m : ms) got[fs::path(m.from).filename().string()] = {m.rule, m.to};
        const auto Y = [&](std::int64_t t) { std::time_t tt = t; std::tm tm{}; ::localtime_r(&tt, &tm); return std::to_string(tm.tm_year + 1900); };
        std::string list;
        for (const auto& [k, v] : got) list += k + ":" + std::to_string(v.first) + " ";
        check("rules: first match wins -- the invoice to Invoices (not PDFs), manual too young, the old iso, the old zip archived",
              got.size() == 3 && got.count("Invoice-March.PDF") && got["Invoice-March.PDF"].first == 0 &&
              got.count("setup.iso") && got["setup.iso"].first == 2 && got.count("old.zip") && got["old.zip"].first == 3 &&
              got.count("arriving.pdf") == 0, list);
        check("rules: never -- partial, hidden, in a subfolder, still arriving, a rule that's off",
              !got.count("movie.mp4.part") && !got.count(".hidden.pdf") && !got.count("deep.pdf") && !got.count("arriving.pdf") &&
              !got.count("Font.otf"));
        check("rules: where -- renamed into a dated folder; the archive by year",
              got["Invoice-March.PDF"].second == H + "/Documents/Invoices/" + Y(now - 10 * D) + "/" +
                  core::organize::stamp(now - 10 * D, false) + "_Invoice-March.PDF" &&
              got["old.zip"].second == H + "/Archive/" + Y(now - 400 * D) + "/Downloads/old.zip", got["Invoice-March.PDF"].second);
        {
            const auto one = rl::matches(H, book.rules, now, std::size_t(4));
            check("rules: the edit preview -- one rule, even while off", one.size() == 1 && fs::path(one[0].from).filename() == "Font.otf");
        }

        dr::Door door(H, data);
        door.load();
        age("Downloads/setup.iso", now - 61 * D);   // changed since -> left out
        std::vector<std::string> dropped;
        auto p = rl::plan(H, book.rules, ms, dropped);
        std::size_t mk = 0, mv = 0, tr = 0;
        for (const auto& a : p.actions) { mk += a.verb == dr::Verb::MakeDir; mv += a.verb == dr::Verb::Move; tr += a.verb == dr::Verb::Trash; }
        check("rules: plan -- folders made first, 2 moves, the changed iso left out", mk == 5 && mv == 2 && tr == 0 &&
              dropped.size() == 1 && p.actions.front().verb == dr::Verb::MakeDir, std::to_string(mk) + "/" + std::to_string(mv));
        check("rules: the door refuses it unpreviewed", !door.run(p).ok);
        const std::string b1 = files_snapshot(home);
        p.previewed = true;
        auto r = door.run(p);
        check("rules: run -- the invoice renamed into place, the zip archived",
              r.ok && fs::exists(got["Invoice-March.PDF"].second) && fs::exists(got["old.zip"].second) &&
              !fs::exists(home / "Downloads/old.zip"), r.messages.empty() ? "" : r.messages.front());
        auto u = door.undo();
        check("rules: undo -- exact outside the Trash", u.ok && [&] {
            std::istringstream a(files_snapshot(home)), b(b1);
            std::string x, y, l;
            while (std::getline(a, l)) if (l.find("/.local/share/Trash/") == std::string::npos) x += l + "\n";
            while (std::getline(b, l)) if (l.find("/.local/share/Trash/") == std::string::npos) y += l + "\n";
            return x == y;
        }());
        {   // Trash, and File into a project
            const auto m2 = rl::matches(H, book.rules, now);
            std::vector<std::string> d2;
            auto p2 = rl::plan(H, book.rules, m2, d2);
            p2.previewed = true;
            bool trashed = false;
            for (const auto& a : p2.actions) trashed |= a.verb == dr::Verb::Trash && a.from == H + "/Downloads/setup.iso";
            r = door.run(p2);
            check("rules: the iso goes to the Trash", trashed && r.ok && fs::exists(home / ".local/share/Trash/files/setup.iso"));
            door.undo();
        }
        {
            fs::create_directories(home / "Documents/Jobs/7_X/fonts", ec);
            put_bytes(home, "Documents/Jobs/7_X/.rouster.json",
                      R"({"kind":"project","id":"7","title":"X","pattern":"{id}_{title}_{yyyymmdd}_{part}.{ext}","routes":{"fonts":["otf"]}})");
            rl::Rule fi; fi.name = "Fonts to job 7"; fi.exts = {"otf"}; fi.act = rl::Do::File; fi.dest = "Documents/Jobs/7_X"; fi.enabled = true;
            const auto m3 = rl::matches(H, {fi}, now);
            check("rules: File into a project -- its pattern and route",
                  rl::problem(fi, H).empty() && m3.size() == 1 &&
                  m3[0].to == H + "/Documents/Jobs/7_X/fonts/7_X_" + core::organize::stamp(now - D, false) + "_Font.otf",
                  m3.empty() ? "" : m3[0].to);
        }
        {
            const auto st = rl::starters();
            check("rules: starters -- five, all off, each one usable once its folder exists",
                  st.size() == 5 && std::none_of(st.begin(), st.end(), [](auto& x) { return x.enabled; }) &&
                  rl::problem(st[0], H).empty(), rl::problem(st[0], H));
        }
        fs::remove_all(home, ec);
    }

    // -- Profile backup: find (read-only) -> plan (MakeDir + Copy + Write) -> door (s012a)
    {
        std::cout << "\n-- profile --\n";
        namespace pr = core::profile;
        namespace dr = core::door;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_profile";
        std::error_code ec;
        fs::remove_all(home, ec);
        fs::create_directories(home / ".local/share/Trash/files", ec);
        fs::create_directories(home / ".local/share/Trash/info", ec);
        fs::create_directories(home / ".local/share/rouster", ec);
        const std::string H = home.string();
        check("profile: fixture", make_profile_fixture(home));
        put(home, "fixes/.bashrc", 10);          // never gathered: not a dotfile path
        put(home, "Documents/letter.odt", 3000); // your files: not a profile

        std::vector<std::vector<std::string>> asked;
        std::string dconf_out = "[org/gnome/desktop/interface]\ncolor-scheme='prefer-dark'\n";
        const pr::Runner fake = [&](const std::vector<std::string>& a) {
            asked.push_back(a);
            core::vcs::Run r;
            if (a[0] == "dconf") { r.code = 0; r.out = dconf_out; }
            else if (a[0] == "dnf") { r.code = 127; }
            else if (a[0] == "flatpak") { r.code = 0; r.out = "org.gnome.Fixture\tflathub\tuser\n"; }
            return r;
        };
        check("profile: bundle name -- host and minute, unsafe characters made safe",
              pr::bundle_name("my box/1", 0).rfind("my-box-1-", 0) == 0 && pr::bundle_name("", 0).rfind("host-", 0) == 0);

        const std::string before = files_snapshot(home);
        pr::Found f = pr::find(H, fake);
        check("profile: find changes nothing", files_snapshot(home) == before && f.refused.empty());
        std::set<std::string> dests;
        for (const auto& it : f.items) dests.insert(it.dest);
        const std::set<std::string> want = {"home/.bashrc", "home/.gitconfig", "home/.ssh/config", "gnome/dconf.ini",
            "gnome/extensions.txt", "home/.config/autostart/notes.desktop", "home/.config/systemd/user/sync.service",
            "units/enabled.txt", "home/.local/share/fonts/Fixture Sans.otf", "packages/flatpak.txt"};
        std::string got;
        for (const auto& d : dests) got += d + " ";
        check("profile: exactly the set-up -- dotfiles, dconf, extensions list, autostart, units, fonts, flatpak",
              dests == want, got);
        const auto item = [&](const std::string& d) -> pr::Item* {
            for (auto& it : f.items) if (it.dest == d) return &it;
            return nullptr;
        };
        check("profile: never the ssh key, never the extension's files, never your documents",
              !dests.count("home/.ssh/id_ed25519") && !dests.count("home/Documents/letter.odt") &&
              !dests.count("home/.local/share/gnome-shell/extensions/dash@fixture/metadata.json"));
        check("profile: ~/.ssh/config is READ (protected folder), the rest are copied",
              item("home/.ssh/config") && item("home/.ssh/config")->read && item("home/.bashrc") &&
              !item("home/.bashrc")->read && !item("home/.bashrc")->made);
        check("profile: the extension listed by uuid and name",
              item("gnome/extensions.txt") && item("gnome/extensions.txt")->content.find("dash@fixture\tDash Fixture\t7") != std::string::npos);
        check("profile: the enabled unit listed from its link",
              item("units/enabled.txt") &&
              item("units/enabled.txt")->content.find("default.target.wants/sync.service -> ../sync.service") != std::string::npos);
        bool cache_only = false;
        for (const auto& a : asked) if (a[0] == "dnf") cache_only = a.size() > 1 && a[1] == "-C";
        check("profile: dnf asked cache-only; missing dnf is a note, not a failure",
              cache_only && std::find(f.notes.begin(), f.notes.end(), "dnf: not installed") != f.notes.end());
        check("profile: everything pre-checked", std::all_of(f.items.begin(), f.items.end(), [](auto& it) { return it.checked; }));

        dr::Door door(H, H + "/.local/share/rouster");
        door.load();
        item("home/.gitconfig")->checked = false;
        const std::int64_t now = 1800000000;
        std::string bundle;
        std::vector<std::string> dropped;
        dr::Plan p = pr::plan(H, f, "custom Nautilus: rebuild from ~/Develop/nautilus", now, bundle, dropped);
        std::size_t mk = 0, cp = 0, wr = 0;
        for (const auto& a : p.actions)
            (a.verb == dr::Verb::MakeDir ? mk : a.verb == dr::Verb::Copy ? cp : wr)++;
        const std::string B = H + "/Archive/Profile/" + pr::bundle_name(f.host, now);
        check("profile: plan -- ~/Archive, Profile, the bundle first; then its folders; a Copy per file, a Write per text",
              p.actions.size() > 3 && p.actions[0].to == H + "/Archive" && p.actions[1].to == H + "/Archive/Profile" &&
              p.actions[2].to == B && bundle == B && cp == 4 && wr == 7 && dropped.empty(),
              std::to_string(mk) + " mk, " + std::to_string(cp) + " cp, " + std::to_string(wr) + " wr");
        check("profile: the unchecked item is not in the plan",
              std::none_of(p.actions.begin(), p.actions.end(), [](auto& a) { return a.to.ends_with("/.gitconfig"); }));
        check("profile: the door refuses it unpreviewed", !door.run(p).ok && files_snapshot(home) == before);
        p.previewed = true;
        auto r = door.run(p);
        const auto read_all = [](const fs::path& x) {
            std::ifstream in(x, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        check("profile: run -> the bundle holds each file's exact bytes, the dump, the notes, PROFILE.txt",
              r.ok && read_all(B + "/home/.bashrc") == read_all(home / ".bashrc") &&
              read_all(B + "/home/.ssh/config") == read_all(home / ".ssh/config") &&
              read_all(B + "/home/.local/share/fonts/Fixture Sans.otf") == read_all(home / ".local/share/fonts/Fixture Sans.otf") &&
              read_all(B + "/gnome/dconf.ini") == dconf_out && read_all(B + "/NOTES.md").find("Nautilus") != std::string::npos &&
              read_all(B + "/PROFILE.txt").find("home/.ssh/config  <-  ~/.ssh/config") != std::string::npos &&
              read_all(B + "/PROFILE.txt").find("dnf: not installed") != std::string::npos,
              r.messages.empty() ? "" : r.messages.front());
        const auto outside = [](const std::string& snap, const char* skip) {
            std::istringstream in(snap);
            std::string o;
            for (std::string l; std::getline(in, l);)
                if (l.find(skip) == std::string::npos) o += l + "\n";
            return o;
        };
        check("profile: ...and every source is exactly as it was", outside(files_snapshot(home), "/Archive") == before);
        check("profile: it is one step", door.batches().size() == 1);

        pr::Found f2 = pr::find(H, fake);
        check("profile: Look again lists the bundle", f2.bundles.size() == 1 && f2.bundles[0] == pr::bundle_name(f.host, now));
        std::string b2;
        std::vector<std::string> d2;
        dr::Plan p2 = pr::plan(H, f2, "", now, b2, d2);
        check("profile: same minute again -> -2, and no MakeDir for what is there",
              b2 == B + "-2" && p2.actions[0].to == B + "-2" &&
              std::none_of(p2.actions.begin(), p2.actions.end(), [](auto& a) { return a.to.ends_with("/NOTES.md"); }));

        auto u = door.undo();
        check("profile: undo -> exact (the bundle, emptied, went to the Trash)",
              u.ok && outside(files_snapshot(home), "/.local/share/Trash/") == outside(before, "/.local/share/Trash/"),
              u.messages.empty() ? "" : u.messages.front());

        put_bytes(home, ".bashrc", "# changed after Look\n");
        std::string b3;
        std::vector<std::string> d3;
        dr::Plan p3 = pr::plan(H, f, "", now, b3, d3);
        check("profile: a file changed since Look is left out, and said",
              d3.size() == 1 && d3[0].find(".bashrc") != std::string::npos &&
              std::none_of(p3.actions.begin(), p3.actions.end(), [](auto& a) { return a.to.ends_with("/.bashrc"); }));

        dconf_out.assign(dr::kWriteMax + 1, 'x');
        pr::Found f4 = pr::find(H, fake);
        bool no_dump = true;
        for (const auto& it : f4.items) if (it.dest == "gnome/dconf.ini") no_dump = false;
        check("profile: a dump too big to write is left out with a note", no_dump && !f4.notes.empty());
        check("profile: refused -- not a home", !pr::find(H + "/nope", fake).refused.empty());

        // -- Restore (s012b): look back at a bundle, diff, Trash + Copy, undo.
        dconf_out = "[org/gnome/desktop/interface]\ncolor-scheme='prefer-dark'\n";
        pr::Found f5 = pr::find(H, fake);
        std::string B5;
        std::vector<std::string> d5;
        dr::Plan p5 = pr::plan(H, f5, "", now + 600, B5, d5);
        p5.previewed = true;
        check("restore: a fresh backup to restore from", door.run(p5).ok && pr::bundles(H).size() == 1, B5);
        put_bytes(home, ".bashrc", "# fixture bashrc\nalias ll='ls -la'\n");
        fs::remove_all(home / ".config/autostart", ec);
        put_bytes(home, ".ssh/config", "Host gh\n  HostName example.org\n");
        const pr::Runner fake2 = [&](const std::vector<std::string>& a) {
            if (a[0] == "diff") return core::vcs::child(a, 10);
            core::vcs::Run r;
            if (a[0] == "dconf") { r.code = 0; r.out = dconf_out; }
            else if (a[0] == "flatpak") { r.code = 0; r.out = "org.other.App\n"; }
            else r.code = 127;
            return r;
        };
        {
            const std::string ssh_now = read_all(home / ".ssh/config");
            put_bytes(home, ".ssh/config", read_all(B5 + "/home/.ssh/config"));
            pr::Restore same = pr::look_back(H, B5, fake2);
            bool ok = false;
            for (const auto& b : same.files) if (b.rel == ".ssh/config") ok = b.state == pr::State::Same;
            std::string c2;
            for (const auto& c : same.commands) c2 += c.line;
            check("restore: a protected file that is the same is just Same -- no line to run", ok && c2.find(".ssh") == std::string::npos);
            put_bytes(home, ".ssh/config", ssh_now);
        }
        {
            const fs::path link = home / ".config/systemd/user/default.target.wants/sync.service";
            fs::remove(link, ec);
            std::string c3;
            for (const auto& c : pr::look_back(H, B5, fake2).commands) c3 += c.line + "\n";
            check("restore: the service's link gone from home -> 'systemctl --user enable sync.service'",
                  c3.find("systemctl --user enable sync.service") != std::string::npos, c3);
            fs::create_symlink("../sync.service", link, ec);
        }
        const std::string pre = files_snapshot(home);
        pr::Restore rb = pr::look_back(H, B5, fake2);
        check("restore: look changes nothing", files_snapshot(home) == pre && rb.refused.empty() && rb.host == f5.host);
        const auto row = [&](const std::string& rel) -> pr::Back* {
            for (auto& b : rb.files) if (b.rel == rel) return &b;
            return nullptr;
        };
        check("restore: states -- bashrc differs (unchecked), autostart missing (checked), font same, ssh config protected",
              row(".bashrc") && row(".bashrc")->state == pr::State::Differs && !row(".bashrc")->checked &&
              row(".config/autostart/notes.desktop") && row(".config/autostart/notes.desktop")->state == pr::State::Missing &&
              row(".config/autostart/notes.desktop")->checked &&
              row(".local/share/fonts/Fixture Sans.otf") && row(".local/share/fonts/Fixture Sans.otf")->state == pr::State::Same &&
              row(".ssh/config") && row(".ssh/config")->state == pr::State::Protected);
        check("restore: the difference shown as diff -u",
              row(".bashrc") && row(".bashrc")->diff.find("-alias ll='ls -la'") != std::string::npos &&
              row(".bashrc")->diff.find("+# changed after Look") != std::string::npos, row(".bashrc") ? row(".bashrc")->diff : "");
        std::string cmds;
        for (const auto& c : rb.commands) cmds += c.line + "\n";
        check("restore: commands -- ssh config by hand, the missing flatpak; dconf the same -> no line; "
              "the service enabled now -> no systemctl line (s012c)",
              cmds.find("cp -i ") != std::string::npos && cmds.find(".ssh/config") != std::string::npos &&
              cmds.find("flatpak install -y --user 'flathub' 'org.gnome.Fixture'") != std::string::npos &&
              cmds.find("systemctl") == std::string::npos &&
              cmds.find("dconf load") == std::string::npos, cmds);
        check("restore: refused -- not a bundle", !pr::look_back(H, H + "/Documents", fake2).refused.empty() &&
              !pr::look_back(H, B5 + "/../" + pr::bundle_name(f5.host, now + 600), fake2).refused.empty());

        row(".bashrc")->checked = true;
        row(".ssh/config")->checked = true;   // forced: still never planned
        std::vector<std::string> d6;
        dr::Plan p6 = pr::restore_plan(H, rb, d6);
        std::string steps;
        for (const auto& a : p6.actions) steps += std::string(dr::verb_name(a.verb)) + " " + (a.to.empty() ? a.from : a.to) + ", ";
        check("restore: plan -- make ~/.config/autostart, Trash the bashrc there, Copy the backup's, Copy the missing one",
              p6.actions.size() == 4 && p6.actions[0].verb == dr::Verb::MakeDir && p6.actions[0].to == H + "/.config/autostart" &&
              p6.actions[1].verb == dr::Verb::Trash && p6.actions[1].from == H + "/.bashrc" &&
              p6.actions[2].verb == dr::Verb::Copy && p6.actions[2].to == H + "/.bashrc" &&
              p6.actions[3].to == H + "/.config/autostart/notes.desktop" && d6.empty(), steps);
        check("restore: the door refuses it unpreviewed", !door.run(p6).ok && files_snapshot(home) == pre);
        p6.previewed = true;
        auto r6 = door.run(p6);
        check("restore: run -> the backup's bashrc and autostart entry are back; ssh config untouched",
              r6.ok && read_all(home / ".bashrc") == read_all(B5 + "/home/.bashrc") &&
              fs::exists(home / ".config/autostart/notes.desktop") &&
              read_all(home / ".ssh/config") == "Host gh\n  HostName example.org\n", r6.messages.empty() ? "" : r6.messages.front());
        auto u6 = door.undo();
        check("restore: undo -> exact (the replaced bashrc back from the Trash)",
              u6.ok && outside(files_snapshot(home), "/.local/share/Trash/") == outside(pre, "/.local/share/Trash/") &&
              read_all(home / ".bashrc") == "# fixture bashrc\nalias ll='ls -la'\n", u6.messages.empty() ? "" : u6.messages.front());
        put_bytes(home, ".bashrc", "# changed again\n");
        std::vector<std::string> d7;
        dr::Plan p7 = pr::restore_plan(H, rb, d7);
        check("restore: changed since the look -> left out, and said",
              d7.size() == 1 && d7[0].find(".bashrc") != std::string::npos && p7.actions.size() == 2);
        dr::Action dup;   // the door: a create may take a trashed path's place only within the same plan
        dup.verb = dr::Verb::Copy;
        dup.from = B5 + "/home/.bashrc";
        dup.to = H + "/.bashrc";
        check("door: a create onto an existing file is still refused without the Trash before it",
              !door.refusal(dup, {}).empty() && door.refusal(dup, {}, {H + "/.bashrc"}).empty());

        // -- A fixture's dconf is the fixture's own (s012d, RULES: fixtures, never the real home).
        {
            const auto av = dr::fixture_dconf_argv(H, {"dconf", "load", "/"});
            check("settings: a fixture's dconf runs with XDG_CONFIG_HOME=<fixture>/.config on a private bus",
                  av.size() == 7 && av[0] == "env" && av[1] == "XDG_CONFIG_HOME=" + H + "/.config" && av[2] == "dbus-run-session" &&
                  av[4] == "dconf");
            const bool tools = core::vcs::child({"dconf", "help"}, 10).code != 127 &&
                               core::vcs::child({"dbus-run-session", "--version"}, 10).code != 127;
            if (tools) {
                const auto real_before = core::vcs::child({"dconf", "dump", "/"}, 20);
                const auto fx = dr::fixture_dconf(H);
                const auto l = fx({"dconf", "load", "/"}, "[org/rouster/selftest]\nfixture=true\n");
                const auto d = fx({"dconf", "dump", "/"}, "");
                const auto via_runner = pr::fixture_runner(H)({"dconf", "dump", "/"});
                const auto real_after = core::vcs::child({"dconf", "dump", "/"}, 20);
                check("settings: (real dconf) a load into the fixture lands in the fixture's database, not yours",
                      l.code == 0 && d.out.find("fixture=true") != std::string::npos &&
                      via_runner.out.find("fixture=true") != std::string::npos && fs::exists(home / ".config/dconf/user") &&
                      real_after.out == real_before.out && real_after.out.find("org/rouster/selftest") == std::string::npos,
                      l.err + d.err);
                fx({"dconf", "reset", "-f", "/org/rouster/selftest/"}, "");
            }
        }

        // -- GNOME settings (s012d): key by key, unchecked, one Settings step, exact undo.
        {
            dconf_out = "[org/gnome/desktop/interface]\ncolor-scheme='default'\nclock-format='24h'\n";
            pr::Restore rs = pr::look_back(H, B5, fake2);
            std::string lines;
            for (const auto& c : rs.commands) lines += c.line;
            check("settings: look -- 1 key would change (shown now / backup), 1 set since is kept, unchecked, no line to run",
                  rs.settings.there && rs.settings.differs && !rs.settings.checked && rs.settings.changes == 1 &&
                  rs.settings.kept == 1 && rs.settings.what.find("color-scheme\n    now:    'default'\n    backup: 'prefer-dark'") !=
                                               std::string::npos &&
                  lines.find("dconf") == std::string::npos, rs.settings.what);
            for (auto& b : rs.files) b.checked = false;
            std::vector<std::string> ds;
            check("settings: unchecked -> not in the plan", pr::restore_plan(H, rs, ds).actions.empty());
            rs.settings.checked = true;
            dr::Plan ps = pr::restore_plan(H, rs, ds);
            check("settings: checked -> ONE Settings action, the bundle's dump",
                  ps.actions.size() == 1 && ps.actions[0].verb == dr::Verb::Settings &&
                  ps.actions[0].content.find("prefer-dark") != std::string::npos && door.refusal(ps.actions[0]).empty());
            auto fd = std::make_shared<FakeDconf>();
            fd->keys = {{"/org/gnome/desktop/interface/color-scheme", "'default'"},
                        {"/org/gnome/desktop/interface/clock-format", "'24h'"}};
            const auto k0 = fd->keys;
            door.set_dconf([fd](const std::vector<std::string>& a, const std::string& in) { return (*fd)(a, in); });
            const std::string files0 = outside(files_snapshot(home), "/.local/share/rouster");
            ps.previewed = true;
            const auto r = door.run(ps);
            check("settings: run -> dark again, the clock you set since kept, no file of yours touched",
                  r.ok && fd->keys.at("/org/gnome/desktop/interface/color-scheme") == "'prefer-dark'" &&
                  fd->keys.at("/org/gnome/desktop/interface/clock-format") == "'24h'" &&
                  outside(files_snapshot(home), "/.local/share/rouster") == files0, r.messages.empty() ? "" : r.messages.front());
            const auto u = door.undo();
            check("settings: undo -> exactly as before", u.ok && fd->keys == k0, u.messages.empty() ? "" : u.messages.front());
            dconf_out = read_all(B5 + "/gnome/dconf.ini");
            pr::Restore same = pr::look_back(H, B5, fake2);
            bool noted = false;
            for (const auto& n : same.notes) if (n.find("GNOME settings") != std::string::npos) noted = true;
            check("settings: the same as now -> not offered, a note", same.settings.there && !same.settings.differs && noted);
        }
        fs::remove_all(home, ec);
    }


    // -- Secrets (s012c): tar in memory, gpg --symmetric, open, compare ------
    {
        std::cout << "\n-- secrets --\n";
        namespace sc = core::secrets;
        namespace pr = core::profile;
        namespace dr = core::door;
        std::error_code ec;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_secrets";
        fs::remove_all(home, ec);
        const std::string H = home.string();
        check("secrets: fixture", make_profile_fixture(home));
        put_bytes(home, ".ssh/known_hosts", "github.com ssh-ed25519 AAAAfixture\n");
        put_bytes(home, ".ssh/id_ed25519.pub", "ssh-ed25519 AAAAfixture me@box\n");
        put_bytes(home, ".gnupg/private-keys-v1.d/ABCD.key", std::string("\x00\x01binary\xff key", 15));
        put_bytes(home, ".gnupg/pubring.kbx", "kbx");
        put_bytes(home, ".gnupg/.#lk0x1234.box.99", "lock");
        put_bytes(home, ".gnupg/pubring.kbx.lock", "lock");
        put_bytes(home, ".local/share/keyrings/login.keyring", "GnomeKeyring\n");
        fs::create_symlink("/etc/hostname", home / ".ssh/linked", ec);
        fs::permissions(home / ".ssh/id_ed25519", fs::perms::owner_read | fs::perms::owner_write, ec);
        fs::permissions(home / ".gnupg", fs::perms::owner_all, ec);
        fs::create_directories(home / ".local/share/rouster", ec);   // Rouster's own (the door's manifest lives here)

        // tar
        {
            std::vector<sc::Entry> in = {{".ssh", true, 0700, 1700000000, ""},
                                         {".ssh/id", false, 0600, 1700000001, std::string("a\0b\xff", 4)},
                                         {".ssh/" + std::string(80, 'd'), true, 0700, 0, ""},
                                         {".ssh/" + std::string(80, 'd') + "/" + std::string(60, 'f'), false, 0644, 5, "x"},
                                         {".ssh/empty", false, 0600, 5, ""}};
            const std::string t = sc::tar(in);
            std::vector<sc::Entry> out;
            std::string why;
            bool same = sc::untar(t, out, why) && out.size() == in.size();
            for (std::size_t i = 0; same && i < in.size(); ++i)
                same = out[i].path == in[i].path && out[i].dir == in[i].dir && out[i].mode == in[i].mode &&
                       out[i].mtime == in[i].mtime && out[i].bytes == in[i].bytes;
            check("secrets: tar round trip -- folders, modes, times, binary bytes, a long path (ustar prefix)", same, why);
            std::string bad = t;
            bad[600] ^= 0x10;   // inside the second header
            check("secrets: a damaged header is refused", !sc::untar(bad, out, why) && why.find("damaged") != std::string::npos);
            std::string dots = sc::tar({{"../escape", false, 0600, 0, "x"}});
            check("secrets: a path that leaves home is refused", !sc::untar(dots, out, why) && why.find("leaves") != std::string::npos);
            check("secrets: an archive cut short is refused", !sc::untar(t.substr(0, 700), out, why));
            const fs::path tf = fs::temp_directory_path() / "rouster_selftest_secrets.tar";
            put_bytes(tf.parent_path(), tf.filename().string(), t);
            const auto lst = core::vcs::child({"tar", "-tvf", tf.string()}, 10);
            fs::remove(tf, ec);
            check("secrets: GNU tar reads our archive (the by-hand line works)",
                  lst.code == 127 || (lst.code == 0 && lst.out.find(".ssh/id") != std::string::npos &&
                                      lst.out.find(std::string(60, 'f')) != std::string::npos &&
                                      lst.out.find("-rw-------") != std::string::npos), lst.err + lst.out);
        }

        // feed: stdin and fd 3, and a child that stops reading early
        {
            const auto a = core::vcs::feed({"cat"}, "hello stdin", "", 10);
            const auto b = core::vcs::feed({"sh", "-c", "cat <&3"}, "", "on fd three", 10);
            const auto c = core::vcs::feed({"sh", "-c", "head -c 3; cat <&3 >/dev/null"}, std::string(4 << 20, 'z'), "pw", 10);
            const auto d = core::vcs::feed({"sh", "-c", "read x; echo \"[$x]\"; cat <&3"}, "line\n", "", 10);
            check("feed: stdin reaches the child; fd 3 reaches it; a child that stops reading doesn't take Rouster down",
                  a.code == 0 && a.out == "hello stdin" && b.code == 0 && b.out == "on fd three" && c.code == 0 &&
                  c.out == "zzz" && d.out == "[line]\n", c.err);
        }

        check("secrets: passphrase rules -- empty, short, once, mismatched, fine",
              !sc::passphrase_problem("", "").empty() && !sc::passphrase_problem("short", "short").empty() &&
              !sc::passphrase_problem("long enough", "").empty() &&
              !sc::passphrase_problem("long enough", "long enougH").empty() &&
              sc::passphrase_problem("long enough", "long enough").empty());

        const std::string before = files_snapshot(home);
        const sc::Found sf = sc::find(H);
        std::string got;
        for (const auto& x : sf.files) got += x + " ";
        check("secrets: find lists ~/.ssh, ~/.gnupg, keyrings -- no lock files, the link noted not followed",
              std::find(sf.files.begin(), sf.files.end(), ".ssh/id_ed25519") != sf.files.end() &&
              std::find(sf.files.begin(), sf.files.end(), ".gnupg/private-keys-v1.d/ABCD.key") != sf.files.end() &&
              std::find(sf.files.begin(), sf.files.end(), ".local/share/keyrings/login.keyring") != sf.files.end() &&
              got.find("lock") == std::string::npos && got.find(".#lk") == std::string::npos &&
              got.find("linked") == std::string::npos && !sf.notes.empty() && sf.notes[0].find("linked") != std::string::npos &&
              sf.per_root[0] == 4 && sf.per_root[1] == 2 && sf.per_root[2] == 1, got);
        check("secrets: find changes nothing", files_snapshot(home) == before);
        pr::Found pf = pr::find(H, [](const std::vector<std::string>&) { core::vcs::Run r; r.code = 127; return r; });
        check("secrets: the profile Look carries the list", pf.secrets.files == sf.files);

        const bool have_gpg = core::vcs::child({"gpg", "--version"}, 10).code == 0;
        if (!have_gpg) {
            check("secrets: gpg not installed -- pack says so", sc::pack(H, sf, "a passphrase", sc::real_gpg()).error.find("gpg") != std::string::npos);
        } else {
            std::vector<std::vector<std::string>> argvs;
            const sc::Gpg rec = [&](const std::vector<std::string>& a, const std::string& in, const std::string& pass) {
                argvs.push_back(a);
                return core::vcs::feed(a, in, pass, 120);
            };
            const std::string pass = "correct horse battery";
            const std::string gnupg_before = files_snapshot(home / ".gnupg");
            const auto pk = sc::pack(H, sf, pass, rec);
            bool leak = false;
            for (const auto& a : argvs) for (const auto& s : a) if (s.find(pass) != std::string::npos) leak = true;
            check("secrets: pack -> encrypted bytes, checked to open again", pk.error.empty() && !pk.data.empty() && pk.files == 7 &&
                  argvs.size() == 2, pk.error);
            check("secrets: the passphrase is never on gpg's argv; gpg runs in Rouster's own homedir",
                  !leak && !argvs.empty() &&
                  std::find(argvs[0].begin(), argvs[0].end(), sc::gpg_home(H)) != argvs[0].end() &&
                  std::find(argvs[0].begin(), argvs[0].end(), "--passphrase-fd") != argvs[0].end());
            check("secrets: no plaintext in the bytes (the key, the keyring)",
                  pk.data.find("NOT A REAL KEY") == std::string::npos && pk.data.find("GnomeKeyring") == std::string::npos);
            check("secrets: your ~/.gnupg is only read", files_snapshot(home / ".gnupg") == gnupg_before);
            check("secrets: pack refuses a short passphrase", !sc::pack(H, sf, "short", rec).error.empty());

            // Into a bundle, through the door.
            const auto fake = [](const std::vector<std::string>& a) {
                core::vcs::Run r;
                r.code = a[0] == "dconf" ? 0 : 127;
                r.out = a[0] == "dconf" ? "[x]\ny=1\n" : "";
                return r;
            };
            pr::Found f = pr::find(H, fake);
            std::string B;
            std::vector<std::string> dropped;
            dr::Plan p = pr::plan(H, f, "", 1762400000, B, dropped, pk.data, pk.files);
            const auto w = std::find_if(p.actions.begin(), p.actions.end(),
                                        [](auto& a) { return a.to.ends_with(std::string("/") + sc::kFile); });
            check("secrets: the plan writes secrets.tar.gpg, 0600", w != p.actions.end() && w->verb == dr::Verb::Write &&
                  w->mode == 0600 && w->content == pk.data);
            dr::Door door(H, H + "/.local/share/rouster");
            p.previewed = true;
            const auto r = door.run(p);
            struct stat st{};
            const bool there = ::lstat((B + "/" + sc::kFile).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600;
            std::ifstream ptxt(B + "/PROFILE.txt");
            std::ostringstream ps;
            ps << ptxt.rdbuf();
            check("secrets: run -> the file in the bundle (0600); PROFILE.txt says how to open it",
                  r.ok && there && ps.str().find("7 files in secrets.tar.gpg") != std::string::npos &&
                  ps.str().find("--skip-old-files") != std::string::npos, r.messages.empty() ? "" : r.messages.front());
            const pr::Restore lb = pr::look_back(H, B, fake);
            check("secrets: look back sees the encrypted file", lb.secrets == pk.data.size());
            check("secrets: no plaintext secret lands in the bundle",
                  !fs::exists(B + "/home/.ssh/id_ed25519") && !fs::exists(B + "/home/.gnupg"));

            const std::string snap = files_snapshot(home);
            const auto wrong = sc::open(H, B, "not the passphrase", sc::real_gpg());
            check("secrets: the wrong passphrase -> 'doesn't open it', nothing listed",
                  wrong.error.find("doesn't open") != std::string::npos && wrong.rows.empty(), wrong.error);
            const auto same = sc::open(H, B, pass, sc::real_gpg());
            bool all_same = same.error.empty() && same.rows.size() == 7 && same.commands.empty();
            for (const auto& row : same.rows) all_same = all_same && row.state == sc::State::Same;
            check("secrets: open -> all 7 the same, no lines to run", all_same, same.error);
            fs::rename(home / ".ssh/id_ed25519", home / "moved_key", ec);
            put_bytes(home, ".ssh/known_hosts", "changed\n");
            const std::string snap2 = files_snapshot(home);
            const auto o = sc::open(H, B, pass, sc::real_gpg());
            const auto st_of = [&](const std::string& rel) {
                for (const auto& row : o.rows) if (row.rel == rel) return std::string(sc::state_name(row.state));
                return std::string("?");
            };
            std::string lines;
            for (const auto& c : o.commands) lines += c.line + "\n";
            check("secrets: key gone -> missing, known_hosts changed -> differs, the rest same",
                  st_of(".ssh/id_ed25519") == "missing" && st_of(".ssh/known_hosts") == "differs" &&
                  st_of(".ssh/config") == "same", lines);
            check("secrets: lines -- unpack with --skip-old-files; unpack beside into a 700 folder",
                  o.commands.size() == 2 && lines.find("tar -xvf - --skip-old-files -C ~") != std::string::npos &&
                  lines.find("mkdir -m 700 ~/'secrets-from-") != std::string::npos, lines);
            check("secrets: open changes nothing", files_snapshot(home) == snap2 && snap != snap2);
            // The line works: run it, passphrase on stdin via loopback, in the fixture as ~.
            const std::string line = "gpg --batch --pinentry-mode loopback --passphrase-fd 3 --homedir " + sc::gpg_home(H) +
                                     " -d '" + B + "/" + sc::kFile + "' | tar -xf - --skip-old-files -C '" + H + "'";
            const auto ran = core::vcs::feed({"sh", "-c", line}, "", pass, 60);
            core::vcs::child({"gpgconf", "--homedir", sc::gpg_home(H), "--kill", "gpg-agent"}, 10);
            std::ifstream kin(home / ".ssh/id_ed25519");
            std::ostringstream ks;
            ks << kin.rdbuf();
            std::ifstream hin(home / ".ssh/known_hosts");
            std::ostringstream hs;
            hs << hin.rdbuf();
            check("secrets: gpg -d | tar --skip-old-files puts the key back (mode 600) and leaves the changed one",
                  ran.code == 0 && ks.str() == "NOT A REAL KEY" && hs.str() == "changed\n" &&
                  (fs::status(home / ".ssh/id_ed25519").permissions() & fs::perms::all) ==
                      (fs::perms::owner_read | fs::perms::owner_write), ran.err);
            auto u = door.undo();
            check("secrets: undo -> the bundle goes to the Trash", u.ok && !fs::exists(B));
        }
        fs::remove_all(home, ec);
    }

    // -- The source gate: no write call outside the door (s004) --------------
    // RULES: "every write goes through it, and the selftest fails on a write
    // that does not." Allowlisted, each for a reason:
    //   Door.cpp      -- it IS the door;
    //   Recents.cpp   -- Rouster's own recents file, in its own data dir;
    //   selftest.cpp  -- it builds and tears down fixtures;
    //   Known.cpp     -- Rouster's own setting (promoted names), in its own data dir;
    //   Settings.cpp  -- Rouster's own files (rules.json), in its own data dir;
    //   Pipe.cpp      -- s012c: write() only into the pipes of the child it made (gpg's stdin, fd 3);
    //   helper/main.cpp -- s013: the ROOT helper; it makes Rouster's snapshot folder and the snapshots.
    //                    pkexec (the way to root) and the btrfs snapshot ioctls are gate tokens:
    //                    only the door may run the helper, only the helper may snapshot;
//   Stones.*      -- the in-memory undo demo: its verbs are NAMED remove /
    //                    rename and touch no disk (goes when a real model
    //                    adopts Undo.hpp);
    //   Registry.*    -- the widget address book; its remove() is a map erase.
    {
        std::cout << "\n-- source gate --\n";
        const fs::path root = ROUSTER_SOURCE_DIR;
        const std::set<std::string> allowed = {"src/core/Door.cpp", "src/core/Recents.cpp", "src/selftest.cpp",
            "src/core/Stones.cpp", "include/core/Stones.hpp", "src/core/Known.cpp", "src/core/Settings.cpp", "src/core/Pipe.cpp", "src/Registry.cpp", "include/Registry.hpp",
            "src/helper/main.cpp"};
        const std::vector<std::string> calls = {"rename", "renameat", "renameat2", "unlink", "unlinkat", "rmdir",
            "remove", "remove_all", "copy_file", "copy", "create_directory", "create_directories", "create_symlink",
            "create_hard_link", "resize_file", "truncate", "write", "fopen", "mkdir", "symlink", "link", "chmod",
            "permissions", "last_write_time"};
        const std::vector<std::string> tokens = {"ofstream", "O_WRONLY", "O_RDWR", "O_CREAT", "std::fstream",
                                                 "FIDEDUPERANGE", "FICLONE",
                                                 "/usr/bin/pkexec", "BTRFS_IOC_SNAP"};
        // One line -> the write calls in it. Comments are dropped; a member
        // call (x.remove(...)) or another namespace's function (registry::
        // remove) is not a filesystem write; ::, std::, fs:: and bare are.
        const auto line_hits = [&](std::string line) {
            std::vector<std::string> out;
            if (const auto c = line.find("//"); c != std::string::npos) line.erase(c);
            for (const auto& t : tokens)
                if (line.find(t) != std::string::npos) out.push_back(t);
            for (const auto& c : calls) {
                for (std::size_t p = line.find(c + "("); p != std::string::npos; p = line.find(c + "(", p + 1)) {
                    const char before = p ? line[p - 1] : ' ';
                    if (std::isalnum(static_cast<unsigned char>(before)) || before == '_' ||
                        before == '.' || before == '>') continue;
                    if (before == ':') {
                        const std::string q = line.substr(0, p);
                        const bool global = q.size() < 3 || !(std::isalnum(static_cast<unsigned char>(q[q.size() - 3])) || q[q.size() - 3] == '_');
                        const bool stdfs = q.ends_with("std::") || q.ends_with("fs::") || q.ends_with("filesystem::");
                        if (!global && !stdfs) continue;
                    }
                    out.push_back(c + "(");
                }
            }
            return out;
        };
        // The gate is only worth its allowlist if it still bites.
        check("source gate: the matcher catches real writes",
              !line_hits("  ::rename(a, b);").empty() && !line_hits("fs::remove_all(p, ec);").empty() &&
              !line_hits("std::filesystem::rename(a, b);").empty() && !line_hits("  unlink(p);").empty() &&
              !line_hits("std::ofstream f(p);").empty() && !line_hits("open(p, O_WRONLY)").empty());
        check("source gate: ...and passes what is not one",
              line_hits("m_rows.remove(*c);").empty() && line_hits("registry::remove(this);").empty() &&
              line_hits("core::recents_remove(m, p);").empty() && line_hits("// ::rename(a, b) in a comment").empty() &&
              line_hits("open(p, O_RDONLY)").empty() && line_hits("std::ifstream in(p);").empty());

        std::vector<std::string> hits;
        std::size_t files = 0;
        for (const char* sub : {"src", "include"}) {
            for (const auto& e : fs::recursive_directory_iterator(root / sub)) {
                const std::string rel = fs::relative(e.path(), root).string();
                const auto ext = e.path().extension();
                if (!e.is_regular_file() || (ext != ".cpp" && ext != ".hpp") || allowed.count(rel)) continue;
                ++files;
                std::ifstream in(e.path());
                std::string line;
                for (int n = 1; std::getline(in, line); ++n)
                    for (const auto& h : line_hits(line)) hits.push_back(rel + ":" + std::to_string(n) + " " + h);
            }
        }
        check("source gate: no write call outside the door", hits.empty(),
              hits.empty() ? std::to_string(files) + " files clean" : hits.front() + (hits.size() > 1 ? " (+" + std::to_string(hits.size() - 1) + ")" : ""));
    }
    // -- Help (s013a2): the text held to the app --------------------------------
    {
        std::cout << "\n-- help --\n";
        namespace hl = core::help;
        bool pages = true;
        std::string missing;
        // Every sidebar page (Shell_zones' m_stack ids) has a topic -- F1 opens it.
        for (const char* id : {"scan", "sweep", "dups", "dev", "projects", "rules", "archive", "preflight", "profile", "activity"})
            if (!hl::find(id)) { pages = false; missing += std::string(id) + " "; }
        check("help: every sidebar page has a topic", pages, missing);
        std::set<std::string> ids;
        bool full = true, unique = true;
        for (const auto& t : hl::topics()) {
            if (!ids.insert(t.id).second) unique = false;
            if (t.title.empty() || t.lead.empty() || t.group.empty() || t.sections.empty()) full = false;
            for (const auto& sec : t.sections) if (sec.paras.empty()) full = false;
        }
        check("help: ids unique; no topic or section empty", unique && full, std::to_string(hl::topics().size()) + " topics");
        check("help: groups stay together (the list shows each once)", [&] {
            std::vector<std::string> seen;
            for (const auto& t : hl::topics()) if (seen.empty() || seen.back() != t.group) seen.push_back(t.group);
            return std::set<std::string>(seen.begin(), seen.end()).size() == seen.size();
        }());
        check("help: search -- empty is all; 'snapshot' finds Preflight + the helper; 'Snapshot PASSWORD' any case; nonsense none",
              hl::search("").size() == hl::topics().size() &&
              [&] { int n = 0; for (auto* t : hl::search("snapshot")) n += t->id == "preflight" || t->id == "helper"; return n == 2; }() &&
              !hl::search("Snapshot PASSWORD").empty() && hl::search("zzqx flarb").empty());
        check("help: an unknown id is no topic", hl::find("nope") == nullptr);
        // The help names the helper's real path and commands -- they mustn't drift.
        const auto* h = hl::find("helper");
        bool path = false, inst = false;
        for (const auto& sec : h->sections) for (const auto& p : sec.paras) {
            if (p.find(core::helper::kHelperPath) != std::string::npos) path = true;
            if (p == "$ ./install.sh --helper") inst = true;
        }
        check("help: the helper topic names its real path and install line", path && inst);
        const auto* pf = hl::find("preflight");
        bool keep = false;
        for (const auto& sec : pf->sections) for (const auto& p : sec.paras)
            if (p.find("newest " + std::to_string(core::helper::kKeepDefault)) != std::string::npos) keep = true;
        check("help: Preflight's keep-N matches the code", keep);
    }
    // -- Shortcuts: the registry paradigm, exercised ---------------------------
    // A seed's claim isn't a fact until a consumer exercises it. The registry's
    // whole promise is that a key and its advertised row can't drift, because
    // both read one list -- so the guard that matters is pure and lives here:
    // no two chords may collide. (App wires from this same list; ShortcutsDialog
    // renders from it.)
    {
        namespace sc = rouster::core;

        check("shortcuts: format_accel renders modifiers + key",
              sc::format_accel("<Ctrl><Shift>n") == "Ctrl+Shift+N",
              sc::format_accel("<Ctrl><Shift>n"));
        check("shortcuts: format_accel maps a named key to its glyph",
              sc::format_accel("<Ctrl>question") == "Ctrl+?",
              sc::format_accel("<Ctrl>question"));

        {
            sc::ShortcutSpec derived; derived.accels = {"<Ctrl>question", "<Ctrl>slash"};
            check("shortcuts: display_keys derives + joins accels",
                  derived.display_keys() == "Ctrl+?  /  Ctrl+/",
                  derived.display_keys());
            sc::ShortcutSpec doc; doc.keys = "Click a recent";
            check("shortcuts: display_keys prefers an explicit key string",
                  doc.display_keys() == "Click a recent");
        }

        check("shortcuts: registry is populated",
              !sc::shortcut_registry().empty(),
              std::to_string(sc::shortcut_registry().size()) + " specs");
        check("shortcuts: registry is accel-collision-free",
              sc::find_accel_collisions().empty(),
              sc::find_accel_collisions().empty()
                  ? "clean" : sc::find_accel_collisions().front());

        // Sections authored A-Z + contiguous: the dialog walks linearly and
        // starts a heading on change, so a stray out-of-order row would split a
        // section into two headings.
        {
            std::vector<std::string> secs;
            for (const auto& r : sc::shortcut_registry())
                if (secs.empty() || secs.back() != r.section) secs.push_back(r.section);
            std::vector<std::string> sorted = secs;
            std::sort(sorted.begin(), sorted.end());
            check("shortcuts: sections are contiguous and A-Z ordered", secs == sorted);
        }

        // The doc-only half of the paradigm: a gesture row documents without
        // binding (no accels), but must still show keys.
        {
            bool ok = true, saw_doc_only = false;
            for (const auto& r : sc::shortcut_registry())
                if (r.action.empty()) {
                    saw_doc_only = true;
                    if (!r.accels.empty() || r.keys.empty()) ok = false;
                }
            check("shortcuts: a doc-only row binds nothing but still displays keys",
                  ok && saw_doc_only);
        }
    }



    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
