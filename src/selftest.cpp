// rouster_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * Scan     -- the read-only walker over a generated fixture home: kinds,
//                 sizes, thread-invariance, nothing written, refusals.
//
//   cmake --build build --target rouster_selftest && ./build/rouster_selftest

#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/Scan.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <utility>

#include <sys/stat.h>
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
    std::cout << "rouster_selftest -- undo, recents, scan, shortcuts\n";

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
