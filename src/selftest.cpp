// rouster_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * Scan     -- the read-only walker over a generated fixture home: kinds,
//                 sizes, thread-invariance, nothing written, refusals.
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
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
}  // namespace

int main(int argc, char** argv) {
    // `--make-fixture DIR`: build the fixture home for an Xvfb drive, then stop.
    if (argc == 3 && std::string(argv[1]) == "--make-fixture") {
        const bool ok = make_fixture(argv[2]);
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
        check("checklist: snapshot is amber until the helper", status_of(cs, "snapshot") == Status::Amber);
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
    // -- Door: the one door, on a fixture home (s004) --------------------------
    {
        std::cout << "\n-- door --\n";
        namespace dr = core::door;
        using dr::Verb;
        const fs::path home = fs::temp_directory_path() / "rouster_selftest_door";
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
        for (Verb v : dr::verbs()) {
            const std::string name = dr::verb_name(v);
            const std::string before = files_snapshot(home);
            const std::size_t batches = door.batches().size();
            const auto a = v == Verb::Trash ? trash(H + "/.cache/mozilla")
                                            : move(H + "/Documents/report.pdf", H + "/Documents/archive/report.pdf");
            const auto r = door.run(plan(name + " one", {a}));
            check("gate/" + name + ": done through the door", r.ok && r.done == 1,
                  r.messages.empty() ? "" : r.messages.front());
            check("gate/" + name + ": one step", door.batches().size() == batches + 1 && door.undo_label() == name + " one");
            check("gate/" + name + ": the tree changed", files_snapshot(home) != before);
            const auto u = door.undo();
            check("gate/" + name + ": undo puts it back exactly", u.ok && files_snapshot(home) == before,
                  u.messages.empty() ? "" : u.messages.front());
        }
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

    // -- The source gate: no write call outside the door (s004) --------------
    // RULES: "every write goes through it, and the selftest fails on a write
    // that does not." Allowlisted, each for a reason:
    //   Door.cpp      -- it IS the door;
    //   Recents.cpp   -- Rouster's own recents file, in its own data dir;
    //   selftest.cpp  -- it builds and tears down fixtures;
    //   Stones.*      -- the in-memory undo demo: its verbs are NAMED remove /
    //                    rename and touch no disk (goes when a real model
    //                    adopts Undo.hpp);
    //   Registry.*    -- the widget address book; its remove() is a map erase.
    {
        std::cout << "\n-- source gate --\n";
        const fs::path root = ROUSTER_SOURCE_DIR;
        const std::set<std::string> allowed = {"src/core/Door.cpp", "src/core/Recents.cpp", "src/selftest.cpp",
            "src/core/Stones.cpp", "include/core/Stones.hpp", "src/Registry.cpp", "include/Registry.hpp"};
        const std::vector<std::string> calls = {"rename", "renameat", "renameat2", "unlink", "unlinkat", "rmdir",
            "remove", "remove_all", "copy_file", "copy", "create_directory", "create_directories", "create_symlink",
            "create_hard_link", "resize_file", "truncate", "write", "fopen", "mkdir", "symlink", "link", "chmod",
            "permissions", "last_write_time"};
        const std::vector<std::string> tokens = {"ofstream", "O_WRONLY", "O_RDWR", "O_CREAT", "std::fstream"};
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
