#include "core/Secrets.hpp"
#include "core/Settings.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>

// Secrets.cpp (s012c) -- see Secrets.hpp. Reads only: lstat to list, open
// O_RDONLY|O_NOFOLLOW to read at the click. The tar is made and read here, in
// memory; gpg runs through vcs::feed with the passphrase on fd 3.

namespace rouster::core::secrets {

namespace {

std::vector<std::string> names_in(const std::string& abs) {
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

bool skip_name(const std::string& n) {
    return n.starts_with(".#lk") || n.ends_with(".lock") || n.ends_with(".lck");
}

// Read a regular file, not through a link; false if it isn't one or is too big.
bool read_file(const std::string& abs, std::string& out, struct stat& st, std::uint64_t max) {
    const int fd = ::open(abs.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<std::uint64_t>(st.st_size) <= max;
    if (ok) {
        out.clear();
        out.reserve(static_cast<std::size_t>(st.st_size));
        char buf[65536];
        for (;;) {
            const ssize_t got = ::read(fd, buf, sizeof buf);
            if (got > 0) out.append(buf, static_cast<std::size_t>(got));
            else { ok = got == 0; break; }
            if (out.size() > max) { ok = false; break; }
        }
    }
    ::close(fd);
    return ok;
}

std::string quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// ── ustar ───────────────────────────────────────────────────────────────────
void octal(char* field, std::size_t width, std::uint64_t v) {   // width includes the NUL
    std::string s(width - 1, '0');
    for (std::size_t i = width - 1; i-- > 0 && v;) { s[i] = static_cast<char>('0' + (v & 7)); v >>= 3; }
    std::memcpy(field, s.data(), width - 1);
    field[width - 1] = '\0';
}

std::uint64_t parse_octal(const char* f, std::size_t width) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width && f[i]; ++i) {
        if (f[i] == ' ') continue;
        if (f[i] < '0' || f[i] > '7') break;
        v = (v << 3) | static_cast<std::uint64_t>(f[i] - '0');
    }
    return v;
}

// name <= 100, or prefix <= 155 '/' name <= 100. false if it can't fit.
bool split_path(const std::string& p, std::string& prefix, std::string& name) {
    if (p.size() <= 100) { prefix.clear(); name = p; return true; }
    for (std::size_t cut = p.find('/'); cut != std::string::npos; cut = p.find('/', cut + 1)) {
        if (cut <= 155 && p.size() - cut - 1 <= 100 && p.size() - cut - 1 > 0) {
            prefix = p.substr(0, cut);
            name = p.substr(cut + 1);
            return true;
        }
    }
    return false;
}

bool safe_rel(const std::string& p) {
    if (p.empty() || p[0] == '/') return false;
    std::size_t at = 0;
    while (at <= p.size()) {
        const auto q = p.find('/', at);
        const std::string part = p.substr(at, q == std::string::npos ? std::string::npos : q - at);
        if (part == ".." || part.empty() || part == ".") return false;
        if (q == std::string::npos) break;
        at = q + 1;
    }
    return true;
}

std::vector<std::string> gpg_base(const std::string& home) {
    return {"gpg", "--batch", "--no-tty", "--quiet", "--homedir", gpg_home(home), "--pinentry-mode", "loopback",
            "--passphrase-fd", "3", "--no-symkey-cache", "--no-permission-warning"};
}

void stop_agent(const std::string& home) {
    vcs::child({"gpgconf", "--homedir", gpg_home(home), "--kill", "gpg-agent"}, 10);
}

std::string first_line(const std::string& s) {
    std::string t = s.substr(0, s.find('\n'));
    return t.size() > 200 ? t.substr(0, 200) : t;
}

}  // namespace

const std::vector<std::string>& roots() {
    static const std::vector<std::string> r = {".ssh", ".gnupg", ".local/share/keyrings"};
    return r;
}

std::string gpg_home(const std::string& home) { return home + "/.local/share/rouster/gnupg"; }

void wipe(std::string& s) {
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

std::string passphrase_problem(const std::string& a, const std::string& b) {
    if (a.empty()) return "Type a passphrase for the secrets, twice.";
    if (a.size() < kMinPass) return "The passphrase needs at least " + std::to_string(kMinPass) + " characters.";
    if (b.empty()) return "Type the passphrase again, in the second box.";
    if (a != b) return "The two passphrases don't match.";
    return {};
}

const char* state_name(State s) {
    switch (s) {
    case State::Same:    return "same";
    case State::Missing: return "missing";
    case State::Differs: return "differs";
    case State::Blocked: return "blocked";
    }
    return "?";
}

Found find(const std::string& home) {
    Found f;
    for (std::size_t ri = 0; ri < roots().size(); ++ri) {
        const std::string& root = roots()[ri];
        struct stat st{};
        if (::lstat((home + "/" + root).c_str(), &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) { f.notes.push_back("~/" + root + " -- not a folder (a link?), left out"); continue; }
        f.dirs.push_back(root);
        const std::function<void(const std::string&, int)> walk = [&](const std::string& rel, int depth) {
            for (const auto& n : names_in(home + "/" + rel)) {
                const std::string r = rel + "/" + n;
                struct stat s{};
                if (::lstat((home + "/" + r).c_str(), &s) != 0) continue;
                if (S_ISSOCK(s.st_mode) || S_ISFIFO(s.st_mode)) continue;          // gpg-agent's sockets
                if (skip_name(n)) continue;
                if (S_ISDIR(s.st_mode)) {
                    if (depth <= 0) { f.notes.push_back("~/" + r + " -- too deep, left out"); continue; }
                    f.dirs.push_back(r);
                    walk(r, depth - 1);
                } else if (S_ISREG(s.st_mode)) {
                    f.files.push_back(r);
                    f.bytes += static_cast<std::uint64_t>(s.st_size);
                    ++f.per_root[ri];
                } else {
                    f.notes.push_back("~/" + r + " -- a link, left out");
                }
            }
        };
        walk(root, 6);
    }
    std::sort(f.files.begin(), f.files.end());
    std::stable_sort(f.dirs.begin(), f.dirs.end(), [](const std::string& a, const std::string& b) {
        return std::count(a.begin(), a.end(), '/') < std::count(b.begin(), b.end(), '/');
    });
    return f;
}

std::string tar(const std::vector<Entry>& entries) {
    std::string out;
    for (const auto& e : entries) {
        std::string prefix, name;
        const std::string path = e.dir ? e.path + "/" : e.path;
        if (!split_path(path, prefix, name)) continue;
        char h[512];
        std::memset(h, 0, sizeof h);
        std::memcpy(h, name.data(), name.size());
        octal(h + 100, 8, e.mode & 07777);
        octal(h + 108, 8, ::getuid());
        octal(h + 116, 8, ::getgid());
        octal(h + 124, 12, e.dir ? 0 : e.bytes.size());
        octal(h + 136, 12, static_cast<std::uint64_t>(std::max<std::int64_t>(0, e.mtime)));
        h[156] = e.dir ? '5' : '0';
        std::memcpy(h + 257, "ustar", 6);    // "ustar\0"
        std::memcpy(h + 263, "00", 2);
        std::memcpy(h + 345, prefix.data(), prefix.size());
        std::memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (unsigned char c : h) sum += c;
        octal(h + 148, 7, sum);              // six digits, NUL, then the space already there
        h[155] = ' ';
        out.append(h, 512);
        if (!e.dir) {
            out += e.bytes;
            out.append((512 - e.bytes.size() % 512) % 512, '\0');
        }
    }
    out.append(1024, '\0');
    return out;
}

bool untar(const std::string& data, std::vector<Entry>& out, std::string& why) {
    out.clear();
    std::size_t at = 0;
    while (at + 512 <= data.size()) {
        const char* h = data.data() + at;
        if (std::all_of(h, h + 512, [](char c) { return c == 0; })) return true;   // the end
        unsigned sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
        if (sum != parse_octal(h + 148, 8)) { why = "a damaged header in the archive"; return false; }
        std::string name(h, strnlen(h, 100)), prefix(h + 345, strnlen(h + 345, 155));
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        const char type = h[156];
        const std::uint64_t size = parse_octal(h + 124, 12);
        at += 512;
        if (at + size > data.size()) { why = "the archive is cut short"; return false; }
        while (!path.empty() && path.back() == '/') path.pop_back();
        if (!safe_rel(path)) { why = "a path that leaves home: " + path; return false; }
        if (type == '0' || type == '\0' || type == '5') {
            Entry e;
            e.path = path;
            e.dir = type == '5';
            e.mode = static_cast<std::uint32_t>(parse_octal(h + 100, 8));
            e.mtime = static_cast<std::int64_t>(parse_octal(h + 136, 12));
            if (!e.dir) e.bytes = data.substr(at, size);
            out.push_back(std::move(e));
        }
        at += size + (512 - size % 512) % 512;
    }
    why = "the archive has no end";
    return false;
}

Gpg real_gpg(const std::atomic<bool>* cancel) {
    return [cancel](const std::vector<std::string>& argv, const std::string& in, const std::string& pass) {
        return vcs::feed(argv, in, pass, 120, cancel);
    };
}

Packed pack(const std::string& home, const Found& found, const std::string& pass, const Gpg& gpg) {
    Packed p;
    if (!passphrase_problem(pass, pass).empty()) { p.error = passphrase_problem(pass, pass); return p; }
    std::vector<Entry> entries;
    std::set<std::string> have_dirs;
    for (const auto& d : found.dirs) {
        struct stat st{};
        if (::lstat((home + "/" + d).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        Entry e;
        e.path = d;
        e.dir = true;
        e.mode = st.st_mode & 07777;
        e.mtime = st.st_mtim.tv_sec;
        entries.push_back(std::move(e));
        have_dirs.insert(d);
    }
    std::uint64_t total = 0;
    for (const auto& rel : found.files) {
        Entry e;
        struct stat st{};
        if (!have_dirs.count(rel.substr(0, rel.rfind('/'))) ||
            !read_file(home + "/" + rel, e.bytes, st, kMax)) {
            p.notes.push_back("~/" + rel + " -- gone or unreadable since Look, left out");
            continue;
        }
        total += e.bytes.size();
        if (total > kMax) {
            for (auto& x : entries) wipe(x.bytes);
            wipe(e.bytes);
            p.error = "the secrets come to more than " + std::to_string(kMax / (1024 * 1024)) +
                      " MB -- too big for one step; see PROFILE.txt for the line to do it by hand";
            return p;
        }
        e.path = rel;
        e.mode = st.st_mode & 07777;
        e.mtime = st.st_mtim.tv_sec;
        entries.push_back(std::move(e));
        ++p.files;
    }
    if (!own_dir(home + "/.local/share/rouster", "gnupg")) {
        for (auto& x : entries) wipe(x.bytes);
        p.error = "could not make Rouster's gpg folder (" + gpg_home(home) + ")";
        return p;
    }
    std::string plain = tar(entries);
    for (auto& x : entries) wipe(x.bytes);
    p.raw = plain.size();

    auto enc = gpg_base(home);
    for (const char* a : {"--symmetric", "--cipher-algo", "AES256", "--output", "-"}) enc.push_back(a);
    auto r = gpg(enc, plain, pass);
    if (r.code != 0 || r.out.empty()) {
        wipe(plain);
        stop_agent(home);
        p.error = r.code == 127 ? "gpg is not installed" : "gpg could not encrypt: " + first_line(r.err);
        return p;
    }
    // Open it once more: a backup that can't be opened is worse than none.
    auto dec = gpg_base(home);
    for (const char* a : {"--decrypt", "--output", "-"}) dec.push_back(a);
    auto back = gpg(dec, r.out, pass);
    stop_agent(home);
    const bool same = back.code == 0 && back.out == plain;
    wipe(back.out);
    wipe(plain);
    if (!same) { p.error = "the encrypted secrets did not open again -- not written (" + first_line(back.err) + ")"; return p; }
    p.data = std::move(r.out);
    return p;
}

Opened open(const std::string& home, const std::string& bundle, const std::string& pass, const Gpg& gpg) {
    Opened o;
    const std::string file = bundle + "/" + kFile;
    std::string cipher;
    struct stat st{};
    if (!read_file(file, cipher, st, 2 * kMax + 1024 * 1024)) { o.error = "no readable " + std::string(kFile) + " in this backup"; return o; }
    if (pass.empty()) { o.error = "Type the passphrase this backup was made with."; return o; }
    if (!own_dir(home + "/.local/share/rouster", "gnupg")) { o.error = "could not make Rouster's gpg folder"; return o; }
    auto dec = gpg_base(home);
    for (const char* a : {"--decrypt", "--output", "-"}) dec.push_back(a);
    auto r = gpg(dec, cipher, pass);
    stop_agent(home);
    if (r.code != 0) {
        wipe(r.out);
        if (r.code == 127) o.error = "gpg is not installed";
        else if (r.err.find("Bad session key") != std::string::npos || r.err.find("decryption failed") != std::string::npos)
            o.error = "That passphrase doesn't open it.";
        else o.error = "gpg could not open it: " + first_line(r.err);
        return o;
    }
    std::vector<Entry> entries;
    std::string why;
    const bool ok = untar(r.out, entries, why);
    wipe(r.out);
    if (!ok) { o.error = why; return o; }

    bool missing = false, differs = false, gnupg = false;
    for (auto& e : entries) {
        if (e.dir) continue;
        Row row;
        row.rel = e.path;
        row.bytes = e.bytes.size();
        const std::string abs = home + "/" + e.path;
        struct stat s{};
        if (::lstat(abs.c_str(), &s) != 0) {
            row.state = errno == ENOENT ? State::Missing : State::Blocked;
        } else if (!S_ISREG(s.st_mode)) {
            row.state = State::Blocked;
        } else {
            std::string now;
            struct stat s2{};
            row.state = read_file(abs, now, s2, kMax) && now == e.bytes ? State::Same : State::Differs;
            wipe(now);
        }
        wipe(e.bytes);
        if (row.state == State::Missing) { missing = true; if (e.path.starts_with(".gnupg/")) gnupg = true; }
        if (row.state == State::Differs) differs = true;
        o.rows.push_back(std::move(row));
    }
    if (missing)
        o.commands.push_back({"Put back the missing ones (--skip-old-files: nothing already there is replaced; gpg asks "
                              "for the passphrase)",
                              "gpg -d " + quote(file) + " | tar -xvf - --skip-old-files -C ~" +
                                  (gnupg ? " && gpgconf --kill gpg-agent" : "")});
    if (differs) {
        const std::string name = bundle.substr(bundle.rfind('/') + 1);
        const std::string side = "~/" + quote("secrets-from-" + name);
        o.commands.push_back({"The ones that differ: unpack beside, in a private folder, compare, and move what you want "
                              "yourself -- then delete that folder",
                              "mkdir -m 700 " + side + " && gpg -d " + quote(file) + " | tar -xvf - -C " + side});
    }
    return o;
}

}  // namespace rouster::core::secrets
