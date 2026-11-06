#include "core/Profile.hpp"
#include "core/Scan.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <cstring>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// Profile.cpp -- READ-ONLY: opendir, lstat, readlink, ifstream, and three
// programs that only print (dconf dump, dnf -C repoquery, flatpak list). The
// door's MakeDir / Copy / Write make the bundle.

namespace rouster::core::profile {

namespace {

using json = nlohmann::json;

std::int64_t ns(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

bool is_protected(const std::string& rel) {
    for (const auto& e : scan::never_touch())
        if (rel == e || rel.rfind(e + "/", 0) == 0) return true;
    std::size_t p = 0;
    while (p <= rel.size()) {
        const auto q = rel.find('/', p);
        const std::string part = rel.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (part == "fixes") return true;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return false;
}

bool slurp(const std::string& abs, std::string& out, std::uint64_t max) {
    std::ifstream in(abs, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return out.size() <= max;
}

std::vector<std::string> list_dir(const std::string& abs) {
    std::vector<std::string> out;
    if (DIR* d = ::opendir(abs.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        ::closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Lines of a program's output, trimmed, blank ones dropped. A literal "\n"
// (dnf 4 does not expand it in --queryformat) splits too.
std::vector<std::string> lines_of(const std::string& text) {
    std::string t;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') { t += '\n'; ++i; }
        else t += text[i];
    }
    std::vector<std::string> out;
    std::istringstream in(t);
    for (std::string l; std::getline(in, l);)
        if (auto s = trim(l); !s.empty()) out.push_back(s);
    return out;
}

std::string first_line(const std::string& s) {
    const auto l = lines_of(s);
    return l.empty() ? std::string() : l.front();
}

struct Gather {
    std::string home;
    Found* out = nullptr;

    // A file at `rel` (must be a regular file) -> one item.
    void file(Group g, const std::string& rel, const std::string& note = {}) {
        struct stat st{};
        const std::string abs = home + "/" + rel;
        if (::lstat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
        Item it;
        it.group = g;
        it.rel = rel;
        it.dest = "home/" + rel;
        it.label = "~/" + rel;
        it.bytes = static_cast<std::uint64_t>(st.st_size);
        it.dev = st.st_dev;
        it.ino = st.st_ino;
        it.mtime_ns = ns(st);
        it.note = note;
        if (is_protected(rel)) {
            // The door never copies out of a Protected folder: read it here, Write it there.
            if (!slurp(abs, it.content, door::kWriteMax)) {
                ++out->errors;
                out->notes.push_back("~/" + rel + ": too big or unreadable, left out");
                return;
            }
            it.read = true;
        } else if (it.bytes > kFileMax) {
            out->notes.push_back("~/" + rel + ": over " + scan::human_bytes(kFileMax) + ", left out");
            return;
        }
        out->items.push_back(std::move(it));
    }

    // Every regular file under `rel`, depth-first, sorted; no symlinks, no
    // hidden entries, `depth` levels at most.
    void tree(Group g, const std::string& rel, int depth) {
        if (depth < 0) return;
        for (const auto& n : list_dir(home + "/" + rel)) {
            if (n[0] == '.') continue;
            const std::string r = rel + "/" + n;
            struct stat st{};
            if (::lstat((home + "/" + r).c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) tree(g, r, depth - 1);
            else if (S_ISREG(st.st_mode)) file(g, r);
        }
    }

    void made(Group g, const std::string& dest, const std::string& label, std::string content,
              const std::string& note = {}) {
        Item it;
        it.group = g;
        it.dest = dest;
        it.label = label;
        it.made = true;
        it.bytes = content.size();
        it.content = std::move(content);
        it.note = note;
        out->items.push_back(std::move(it));
    }
};

std::string stamp(std::int64_t now, bool dashes) {
    std::time_t t = static_cast<std::time_t>(now);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, dashes ? "%Y-%m-%d %H:%M" : "%Y%m%d-%H%M", &tm);
    return b;
}

}  // namespace

const char* group_name(Group g) {
    switch (g) {
    case Group::Dotfiles:  return "Dotfiles";
    case Group::Gnome:     return "GNOME settings";
    case Group::Autostart: return "Autostart";
    case Group::Units:     return "User services";
    case Group::Fonts:     return "Fonts";
    case Group::Packages:  return "Packages";
    }
    return "?";
}

const char* group_why(Group g) {
    switch (g) {
    case Group::Dotfiles:  return "Your shell, git and ssh settings -- the files in home that make a terminal yours.";
    case Group::Gnome:     return "Every GNOME setting as text (keybindings, appearance, extensions' settings), and "
                                  "which extensions you have -- as a list; their files stay where they are.";
    case Group::Autostart: return "What starts when you log in.";
    case Group::Units:     return "systemd user units, and which are enabled.";
    case Group::Fonts:     return "Fonts you installed for yourself.";
    case Group::Packages:  return "What you installed, by name -- to reinstall on a new machine.";
    }
    return "";
}

const std::vector<std::string>& dotfiles() {
    static const std::vector<std::string> v = {
        ".bashrc", ".bash_profile", ".bash_logout", ".profile", ".zshrc", ".zprofile", ".inputrc",
        ".gitconfig", ".gitignore_global", ".vimrc", ".tmux.conf", ".nanorc", ".editorconfig",
        ".ssh/config", ".config/git/config", ".config/git/ignore", ".config/user-dirs.dirs",
        ".config/mimeapps.list", ".config/gtk-3.0/bookmarks", ".config/gtk-3.0/gtk.css",
        ".config/gtk-4.0/gtk.css", ".config/monitors.xml"};
    return v;
}

Runner real_runner(const std::atomic<bool>* cancel) {
    return [cancel](const std::vector<std::string>& argv) { return vcs::child(argv, 60, cancel); };
}

std::string bundle_name(const std::string& host, std::int64_t now) {
    std::string h;
    for (char c : host) h += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.') ? c : '-';
    if (h.empty() || h[0] == '.') h = "host" + h;
    return h + "-" + stamp(now, false);
}

Found find(const std::string& home, const Runner& run) {
    Found f;
    struct stat hs{};
    if (home.empty() || home == "/" || ::stat(home.c_str(), &hs) != 0 || !S_ISDIR(hs.st_mode)) {
        f.refused = "not a home folder: " + home;
        return f;
    }
    char hn[256] = {};
    if (::gethostname(hn, sizeof hn - 1) == 0) f.host = hn;
    if (f.host.empty()) f.host = "host";

    Gather g{home, &f};

    for (const auto& d : dotfiles()) g.file(Group::Dotfiles, d);

    // GNOME: the settings as text; the extensions as a list.
    {
        const auto r = run({"dconf", "dump", "/"});
        if (r.code == 0 && !r.out.empty()) {
            if (r.out.size() > door::kWriteMax) f.notes.push_back("dconf: the dump is over " + scan::human_bytes(door::kWriteMax) + ", left out");
            else g.made(Group::Gnome, "gnome/dconf.ini", "dconf dump /", r.out, "every key you changed");
        } else {
            f.notes.push_back(r.code == 127 ? "dconf: not installed" : "dconf: " + (first_line(r.err).empty() ? "no output" : first_line(r.err)));
        }
        const std::string ext = ".local/share/gnome-shell/extensions";
        std::string text;
        std::size_t n = 0;
        for (const auto& uuid : list_dir(home + "/" + ext)) {
            std::string meta;
            std::string name, version, url;
            if (slurp(home + "/" + ext + "/" + uuid + "/metadata.json", meta, 1 << 20)) {
                const auto j = json::parse(meta, nullptr, false);
                if (j.is_object()) {
                    if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
                    if (j.contains("version") && j["version"].is_number()) version = std::to_string(j["version"].get<long long>());
                    if (j.contains("version-name") && j["version-name"].is_string()) version = j["version-name"].get<std::string>();
                    if (j.contains("url") && j["url"].is_string()) url = j["url"].get<std::string>();
                }
            }
            struct stat st{};
            if (::lstat((home + "/" + ext + "/" + uuid).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            text += uuid + "\t" + name + "\t" + version + "\t" + url + "\n";
            ++n;
        }
        if (n)
            g.made(Group::Gnome, "gnome/extensions.txt", "Extensions (a list)",
                   "# uuid\tname\tversion\turl -- reinstall from extensions.gnome.org\n" + text,
                   std::to_string(n) + (n == 1 ? " extension" : " extensions"));
    }

    // Autostart: the entries at the top of ~/.config/autostart.
    for (const auto& n : list_dir(home + "/.config/autostart"))
        if (n[0] != '.') g.file(Group::Autostart, ".config/autostart/" + n);

    // User units: the files, and the links that say what is enabled (or masked).
    {
        const std::string base = ".config/systemd/user";
        std::string enabled;
        std::size_t links = 0;
        const std::function<void(const std::string&, int)> walk = [&](const std::string& rel, int depth) {
            for (const auto& n : list_dir(home + "/" + rel)) {
                if (n[0] == '.') continue;
                const std::string r = rel + "/" + n;
                struct stat st{};
                if (::lstat((home + "/" + r).c_str(), &st) != 0) continue;
                if (S_ISLNK(st.st_mode)) {
                    char buf[4096];
                    const ssize_t k = ::readlink((home + "/" + r).c_str(), buf, sizeof buf - 1);
                    const std::string to = k > 0 ? std::string(buf, static_cast<std::size_t>(k)) : "?";
                    enabled += r.substr(base.size() + 1) + " -> " + to + "\n";
                    ++links;
                } else if (S_ISDIR(st.st_mode) && depth > 0) {
                    walk(r, depth - 1);
                } else if (S_ISREG(st.st_mode)) {
                    g.file(Group::Units, r);
                }
            }
        };
        walk(base, 2);
        if (links)
            g.made(Group::Units, "units/enabled.txt", "Enabled / masked (a list)",
                   "# <where> -> <unit file>: systemctl --user enable <unit> for each .wants line\n" + enabled,
                   std::to_string(links) + (links == 1 ? " link" : " links"));
    }

    // Fonts.
    g.tree(Group::Fonts, ".local/share/fonts", 6);

    // Packages: names only; cache only (-C), never the network.
    {
        const auto r = run({"dnf", "-C", "repoquery", "--userinstalled", "--queryformat", "%{name}\\n"});
        if (r.code == 0) {
            auto names = lines_of(r.out);
            std::set<std::string> uniq(names.begin(), names.end());
            std::string text;
            for (const auto& n : uniq) text += n + "\n";
            if (!uniq.empty())
                g.made(Group::Packages, "packages/dnf.txt", "dnf: installed by you", text,
                       std::to_string(uniq.size()) + " packages");
            else f.notes.push_back("dnf: nothing listed as installed by you");
        } else {
            f.notes.push_back(r.code == 127 ? "dnf: not installed" : "dnf: " + (first_line(r.err).empty() ? "failed" : first_line(r.err)));
        }
        const auto fl = run({"flatpak", "list", "--app", "--columns=application,origin,installation"});
        if (fl.code == 0) {
            const auto l = lines_of(fl.out);
            std::string text = "# application\torigin\tinstallation -- flatpak install <origin> <application>\n";
            for (const auto& x : l) text += x + "\n";
            if (!l.empty())
                g.made(Group::Packages, "packages/flatpak.txt", "Flatpak apps", text,
                       std::to_string(l.size()) + (l.size() == 1 ? " app" : " apps"));
            else f.notes.push_back("flatpak: no apps installed");
        } else {
            f.notes.push_back(fl.code == 127 ? "flatpak: not installed" : "flatpak: " + (first_line(fl.err).empty() ? "failed" : first_line(fl.err)));
        }
    }

    std::stable_sort(f.items.begin(), f.items.end(),
                     [](const Item& a, const Item& b) { return static_cast<int>(a.group) < static_cast<int>(b.group); });
    for (const auto& it : f.items) f.bytes += it.bytes;

    f.bundles = bundles(home);
    return f;
}

std::string profile_txt(const Found& found, const std::vector<const Item*>& items, std::int64_t now,
                        const std::string& notes) {
    std::string s;
    s += "Rouster profile backup\n";
    s += "host: " + found.host + "\n";
    s += "made: " + stamp(now, true) + "\n";
    s += "items: " + std::to_string(items.size()) + "\n\n";
    s += "A snapshot of how this machine is set up -- not a backup of your files.\n";
    s += "Secrets (ssh keys, gpg, keyrings) are NOT in it.\n\n";
    for (int gi = 0; gi < kGroupCount; ++gi) {
        const auto g = static_cast<Group>(gi);
        bool head = false;
        for (const auto* it : items) {
            if (it->group != g) continue;
            if (!head) { s += "== " + std::string(group_name(g)) + "\n"; head = true; }
            s += it->dest + (it->rel.empty() ? "  (made by Rouster)" : "  <-  ~/" + it->rel) + "  " +
                 scan::human_bytes(it->bytes) + "\n";
        }
        if (head) s += "\n";
    }
    if (!found.notes.empty()) {
        s += "== Not gathered\n";
        for (const auto& n : found.notes) s += n + "\n";
        s += "\n";
    }
    if (!notes.empty()) s += "== Notes: see NOTES.md\n\n";
    s += "== Putting it back by hand (Rouster's own Restore, which shows the differences first, comes later)\n";
    s += "files:      look first, then: cp -an home/. ~/      (-n: never overwrites)\n";
    s += "GNOME:      dconf load / < gnome/dconf.ini           (replaces the keys it names)\n";
    s += "extensions: one by one from extensions.gnome.org (gnome/extensions.txt)\n";
    s += "services:   systemctl --user daemon-reload; then enable each unit in units/enabled.txt\n";
    s += "fonts:      fc-cache -f\n";
    s += "dnf:        sudo dnf install $(grep -v '^#' packages/dnf.txt)\n";
    s += "flatpak:    flatpak install <origin> <application>, per line of packages/flatpak.txt\n";
    return s;
}

door::Plan plan(const std::string& home, const Found& found, const std::string& notes,
                std::int64_t now, std::string& bundle, std::vector<std::string>& dropped) {
    door::Plan p;
    std::string name = bundle_name(found.host, now);
    {
        struct stat st{};
        std::string n = name;
        for (int k = 2; ::lstat((home + "/" + kRoot + "/" + n).c_str(), &st) == 0; ++k) n = name + "-" + std::to_string(k);
        name = n;
    }
    bundle = home + "/" + kRoot + "/" + name;
    p.label = "Profile backup: " + name;

    const auto mk = [&](const std::string& abs, const std::string& why) {
        door::Action a;
        a.verb = door::Verb::MakeDir;
        a.to = abs;
        a.reason = why;
        p.actions.push_back(std::move(a));
    };
    for (const std::string r : {"Archive", "Archive/Profile"}) {
        struct stat st{};
        if (::lstat((home + "/" + r).c_str(), &st) != 0) mk(home + "/" + r, "where profile backups are kept");
    }
    mk(bundle, "this backup");

    // Look at every file again; keep what is unchanged.
    std::vector<const Item*> keep;
    for (const auto& it : found.items) {
        if (!it.checked) continue;
        if (!it.made) {
            struct stat st{};
            const std::string abs = home + "/" + it.rel;
            if (::lstat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { dropped.push_back("~/" + it.rel + " -- gone since Look"); continue; }
            if (st.st_dev != it.dev || st.st_ino != it.ino || ns(st) != it.mtime_ns) {
                dropped.push_back("~/" + it.rel + " -- changed since Look");
                continue;
            }
        }
        keep.push_back(&it);
    }

    // The bundle's folders, parents first.
    std::set<std::string> dirs;
    for (const auto* it : keep) {
        for (auto q = it->dest.find('/'); q != std::string::npos; q = it->dest.find('/', q + 1))
            dirs.insert(it->dest.substr(0, q));
    }
    std::vector<std::string> ordered(dirs.begin(), dirs.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
        return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
    });
    for (const auto& d : ordered) mk(bundle + "/" + d, "a folder in the backup");

    for (const auto* it : keep) {
        door::Action a;
        a.to = bundle + "/" + it->dest;
        a.bytes = it->bytes;
        if (it->made || it->read) {
            a.verb = door::Verb::Write;
            a.content = it->content;
            a.reason = it->made ? it->label : "~/" + it->rel + " (read: it sits in a protected folder)";
        } else {
            a.verb = door::Verb::Copy;
            a.from = home + "/" + it->rel;
            a.reason = "~/" + it->rel;
        }
        p.actions.push_back(std::move(a));
    }
    if (!notes.empty()) {
        door::Action a;
        a.verb = door::Verb::Write;
        a.to = bundle + "/NOTES.md";
        a.content = notes.back() == '\n' ? notes : notes + "\n";
        a.bytes = a.content.size();
        a.reason = "your notes";
        p.actions.push_back(std::move(a));
    }
    door::Action t;
    t.verb = door::Verb::Write;
    t.to = bundle + "/PROFILE.txt";
    t.content = profile_txt(found, keep, now, notes);
    t.bytes = t.content.size();
    t.reason = "what is in this backup, and how to put it back";
    p.actions.push_back(std::move(t));
    return p;
}

// ── Restore (s012b) ─────────────────────────────────────────────────────────

const char* state_name(State st) {
    switch (st) {
    case State::Same:      return "same";
    case State::Missing:   return "missing";
    case State::Differs:   return "differs";
    case State::Protected: return "protected";
    case State::Blocked:   return "blocked";
    }
    return "?";
}

std::vector<std::string> bundles(const std::string& home) {
    std::vector<std::pair<std::int64_t, std::string>> bs;
    for (const auto& n : list_dir(home + "/" + kRoot)) {
        struct stat st{};
        if (::lstat((home + "/" + kRoot + "/" + n + "/PROFILE.txt").c_str(), &st) == 0 && S_ISREG(st.st_mode))
            bs.emplace_back(ns(st), n);
    }
    std::sort(bs.rbegin(), bs.rend());
    std::vector<std::string> out;
    for (auto& [t, n] : bs) out.push_back(n);
    return out;
}

namespace {

constexpr std::uint64_t kDiffMax = 256 * 1024;

bool same_bytes(const std::string& a, const std::string& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return false;
    char bx[65536], by[65536];
    while (true) {
        x.read(bx, sizeof bx);
        y.read(by, sizeof by);
        const auto nx = x.gcount(), ny = y.gcount();
        if (nx != ny || std::memcmp(bx, by, static_cast<std::size_t>(nx)) != 0) return false;
        if (nx == 0) return true;
    }
}

bool texty(const std::string& abs, std::uint64_t size) {
    if (size > kDiffMax) return false;
    std::string c;
    return slurp(abs, c, kDiffMax) && c.find('\0') == std::string::npos;
}

std::string quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

std::string read_text(const std::string& abs) {
    std::string c;
    slurp(abs, c, 4 * 1024 * 1024);
    return c;
}

std::set<std::string> names_of(const std::string& text, std::size_t column) {
    std::set<std::string> out;
    for (const auto& l : lines_of(text)) {
        if (l[0] == '#') continue;
        std::size_t start = 0;
        for (std::size_t c = 0; c < column; ++c) {
            const auto t = l.find('\t', start);
            if (t == std::string::npos) { start = std::string::npos; break; }
            start = t + 1;
        }
        if (start == std::string::npos) continue;
        out.insert(trim(l.substr(start, l.find('\t', start) - start)));
    }
    return out;
}

}  // namespace

Restore look_back(const std::string& home, const std::string& bundle, const Runner& run) {
    Restore r;
    r.bundle = bundle;
    const std::string root = home + "/" + kRoot + "/";
    struct stat st{};
    if (bundle.rfind(root, 0) != 0 || bundle.find("/..") != std::string::npos ||
        bundle.find('/', root.size()) != std::string::npos ||
        ::lstat((bundle + "/PROFILE.txt").c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        r.refused = "not a profile backup: " + bundle;
        return r;
    }
    r.name = bundle.substr(root.size());
    for (const auto& l : lines_of(read_text(bundle + "/PROFILE.txt"))) {
        if (l.rfind("host: ", 0) == 0) r.host = l.substr(6);
        if (l.rfind("made: ", 0) == 0) r.made = l.substr(6);
    }

    // Files.
    bool fonts = false;
    const std::function<void(const std::string&, int)> walk = [&](const std::string& rel, int depth) {
        for (const auto& n : list_dir(bundle + "/home" + (rel.empty() ? "" : "/" + rel))) {
            const std::string rr = rel.empty() ? n : rel + "/" + n;
            const std::string from = bundle + "/home/" + rr;
            struct stat bs{};
            if (::lstat(from.c_str(), &bs) != 0) continue;
            if (S_ISDIR(bs.st_mode)) { if (depth > 0) walk(rr, depth - 1); continue; }
            if (!S_ISREG(bs.st_mode)) continue;
            Back b;
            b.rel = rr;
            b.from = from;
            b.bytes = static_cast<std::uint64_t>(bs.st_size);
            b.from_mtime_ns = ns(bs);
            const std::string now = home + "/" + rr;
            struct stat cs{};
            bool blocked_parent = false;
            for (auto q = rr.find('/'); q != std::string::npos; q = rr.find('/', q + 1)) {
                struct stat ps{};
                if (::lstat((home + "/" + rr.substr(0, q)).c_str(), &ps) == 0 && !S_ISDIR(ps.st_mode)) blocked_parent = true;
            }
            if (is_protected(rr))   // never written; listed (with the line to do it yourself) only when it differs
                b.state = (::lstat(now.c_str(), &cs) == 0 && S_ISREG(cs.st_mode) && same_bytes(now, from)) ? State::Same : State::Protected;
            else if (blocked_parent) b.state = State::Blocked;
            else if (::lstat(now.c_str(), &cs) != 0) { b.state = State::Missing; b.checked = true; }
            else if (!S_ISREG(cs.st_mode)) b.state = State::Blocked;
            else if (cs.st_size == bs.st_size && same_bytes(now, from)) b.state = State::Same;
            else {
                b.state = State::Differs;
                b.ino = cs.st_ino;
                b.mtime_ns = ns(cs);
                if (texty(now, static_cast<std::uint64_t>(cs.st_size)) && texty(from, b.bytes)) {
                    const auto d = run({"diff", "-u", "--label", "now: ~/" + rr, "--label", "backup", now, from});
                    b.diff = d.code == 1 ? d.out : d.code == 127 ? "(diff is not installed)" : "(diff failed)";
                } else {
                    b.diff = "(binary or large -- not shown)";
                }
            }
            if ((b.state == State::Missing || b.state == State::Differs) && rr.rfind(".local/share/fonts/", 0) == 0) fonts = true;
            r.files.push_back(std::move(b));
        }
    };
    walk("", 12);
    for (const auto& b : r.files)
        if (b.state == State::Protected)
            r.commands.push_back({"~/" + b.rel + " -- Rouster never writes in a protected folder",
                                  "cp -i " + quote(b.from) + " ~/" + quote(b.rel)});

    // GNOME settings: until s012d, the line to run.
    if (::lstat((bundle + "/gnome/dconf.ini").c_str(), &st) == 0) {
        const auto d = run({"dconf", "dump", "/"});
        if (d.code == 0 && d.out == read_text(bundle + "/gnome/dconf.ini"))
            r.notes.push_back("GNOME settings: the same as now");
        else
            r.commands.push_back({"GNOME settings (Rouster's own, with Undo, comes in s012d)",
                                  "dconf load / < " + quote(bundle + "/gnome/dconf.ini")});
    }
    if (::lstat((bundle + "/gnome/extensions.txt").c_str(), &st) == 0) {
        std::string missing;
        std::size_t n = 0;
        for (const auto& uuid : names_of(read_text(bundle + "/gnome/extensions.txt"), 0))
            if (::lstat((home + "/.local/share/gnome-shell/extensions/" + uuid).c_str(), &st) != 0) {
                missing += (missing.empty() ? "" : " ") + uuid;
                ++n;
            }
        if (n)
            r.commands.push_back({"Extensions: " + std::to_string(n) + " not installed -- from extensions.gnome.org, or:",
                                  "for e in " + missing + "; do gdbus call --session --dest org.gnome.Shell.Extensions "
                                  "--object-path /org/gnome/Shell/Extensions --method "
                                  "org.gnome.Shell.Extensions.InstallRemoteExtension \"$e\"; done"});
    }

    // User services: enable what was enabled.
    if (::lstat((bundle + "/units/enabled.txt").c_str(), &st) == 0) {
        std::set<std::string> enable, mask;
        for (const auto& l : lines_of(read_text(bundle + "/units/enabled.txt"))) {
            if (l[0] == '#') continue;
            const auto arrow = l.find(" -> ");
            if (arrow == std::string::npos) continue;
            const std::string where = l.substr(0, arrow), to = l.substr(arrow + 4);
            const std::string unit = where.substr(where.rfind('/') + 1);
            if (to == "/dev/null") mask.insert(unit);
            else if (where.find(".wants/") != std::string::npos || where.find(".requires/") != std::string::npos) enable.insert(unit);
        }
        std::string line = "systemctl --user daemon-reload";
        if (!enable.empty()) { line += " && systemctl --user enable"; for (const auto& u : enable) line += " " + u; }
        if (!mask.empty()) { line += " && systemctl --user mask"; for (const auto& u : mask) line += " " + u; }
        if (!enable.empty() || !mask.empty())
            r.commands.push_back({"User services: enable what was enabled (after the files are back)", line});
    }
    if (fonts) r.commands.push_back({"Fonts: after restoring them", "fc-cache -f"});

    // Packages: only what isn't installed now.
    if (::lstat((bundle + "/packages/dnf.txt").c_str(), &st) == 0) {
        const auto want = names_of(read_text(bundle + "/packages/dnf.txt"), 0);
        const auto now = run({"dnf", "-C", "repoquery", "--userinstalled", "--queryformat", "%{name}\\n"});
        std::set<std::string> have;
        if (now.code == 0) for (const auto& n : lines_of(now.out)) have.insert(n);
        else r.notes.push_back("dnf: couldn't list what is installed now -- the line names every package in the backup");
        std::string line;
        std::size_t n = 0;
        for (const auto& w : want) if (!have.count(w)) { line += " " + w; ++n; }
        if (n) r.commands.push_back({"dnf: " + std::to_string(n) + (n == 1 ? " package" : " packages") + " not installed", "sudo dnf install" + line});
        else r.notes.push_back("dnf: every package is installed");
    }
    if (::lstat((bundle + "/packages/flatpak.txt").c_str(), &st) == 0) {
        const auto now = run({"flatpak", "list", "--app", "--columns=application"});
        std::set<std::string> have;
        if (now.code == 0) for (const auto& n : lines_of(now.out)) have.insert(n);
        std::size_t n = 0;
        std::string line;
        for (const auto& l : lines_of(read_text(bundle + "/packages/flatpak.txt"))) {
            if (l[0] == '#') continue;
            std::istringstream in(l);
            std::string app, origin, inst;
            std::getline(in, app, '\t');
            std::getline(in, origin, '\t');
            std::getline(in, inst, '\t');
            app = trim(app);
            if (app.empty() || have.count(app)) continue;
            line += (line.empty() ? "" : " && ") + std::string("flatpak install -y ") +
                    (trim(inst) == "user" ? "--user " : "") + quote(trim(origin)) + " " + quote(app);
            ++n;
        }
        if (n) r.commands.push_back({"Flatpak: " + std::to_string(n) + (n == 1 ? " app" : " apps") + " not installed", line});
        else r.notes.push_back("flatpak: every app is installed");
    }
    return r;
}

door::Plan restore_plan(const std::string& home, const Restore& r, std::vector<std::string>& dropped) {
    door::Plan p;
    p.label = "Restore profile: " + r.name;
    std::vector<const Back*> keep;
    for (const auto& b : r.files) {
        if (!b.checked || (b.state != State::Missing && b.state != State::Differs)) continue;
        struct stat fs_{}, cs{};
        if (::lstat(b.from.c_str(), &fs_) != 0 || !S_ISREG(fs_.st_mode) || ns(fs_) != b.from_mtime_ns) {
            dropped.push_back("~/" + b.rel + " -- the backup's copy changed since the look");
            continue;
        }
        const bool there = ::lstat((home + "/" + b.rel).c_str(), &cs) == 0;
        if (b.state == State::Missing ? there
                                      : (!there || !S_ISREG(cs.st_mode) || cs.st_ino != b.ino || ns(cs) != b.mtime_ns)) {
            dropped.push_back("~/" + b.rel + " -- changed since the look");
            continue;
        }
        keep.push_back(&b);
    }
    std::set<std::string> dirs;
    for (const auto* b : keep)
        for (auto q = b->rel.find('/'); q != std::string::npos; q = b->rel.find('/', q + 1)) {
            const std::string d = b->rel.substr(0, q);
            struct stat st{};
            if (::lstat((home + "/" + d).c_str(), &st) != 0) dirs.insert(d);
        }
    std::vector<std::string> ordered(dirs.begin(), dirs.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const std::string& a, const std::string& b) {
        return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
    });
    for (const auto& d : ordered) {
        door::Action a;
        a.verb = door::Verb::MakeDir;
        a.to = home + "/" + d;
        a.reason = "a folder the backup had";
        p.actions.push_back(std::move(a));
    }
    for (const auto* b : keep) {
        if (b->state == State::Differs) {
            door::Action t;
            t.verb = door::Verb::Trash;
            t.from = home + "/" + b->rel;
            t.reason = "the one there now -- replaced by the backup's (Undo brings it back)";
            p.actions.push_back(std::move(t));
        }
        door::Action c;
        c.verb = door::Verb::Copy;
        c.from = b->from;
        c.to = home + "/" + b->rel;
        c.bytes = b->bytes;
        c.reason = std::string("from the backup (") + state_name(b->state) + " now)";
        p.actions.push_back(std::move(c));
    }
    return p;
}

}  // namespace rouster::core::profile
