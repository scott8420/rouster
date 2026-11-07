// rouster-helper (s013) -- Rouster's small ROOT helper. REQUIREMENTS §1.4:
// "Root work goes through a small polkit helper with a fixed verb list -- no
// arbitrary paths, no rm as root."
//
// Run ONLY through pkexec (the polkit action io.github.scott8420.Rouster.helper,
// installed by `install.sh --helper`):
//
//   pkexec /usr/local/libexec/rouster/rouster-helper run [--dry-run] snapshot:5
//   rouster-helper version
//
// What it trusts: nothing from the app but the request words, each read by
// core::helper::parse (one word + one bounded number). Whose home: pkexec's
// PKEXEC_UID -> the password database, never a path from the command line.
// No shell, ever. The snapshot is two btrfs ioctls; s013b's three verbs run
// dnf / apt-get, journalctl and fstrim by ABSOLUTE path, with a clean
// environment (PATH=/usr/sbin:/usr/bin, LC_ALL=C), stdin from /dev/null, and
// argv fixed here -- the only thing from the request is journal's number.
//
// Output: one line per request on stdout (core::helper::format_line).
// Exit: 0 all ok (or dry), 1 something failed, 2 refused outright.
//
// Allowlisted in the selftest's source gate (it makes folders and snapshots
// as root -- that is its job); nothing else in Rouster may say the btrfs
// snapshot ioctls.
#include "core/Helper.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace h = rouster::core::helper;

namespace {

void say(const std::string& op, const std::string& status, const std::string& detail) {
    std::printf("%s\n", h::format_line({op, status, detail}).c_str());
    std::fflush(stdout);
}

int refuse(const std::string& why) {
    std::fprintf(stderr, "rouster-helper: %s\n", why.c_str());
    return 2;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

// The snapshot verb. true = ok (or dry); detail says what happened.
bool snapshot(const std::string& home, int keep, bool dry, std::string& detail) {
    const h::Place p = h::place_for(home);
    if (!p.ok) { detail = p.why; return false; }
    const std::string name = h::snapshot_name(std::time(nullptr));

    if (dry) {
        auto gone = h::to_expire([&] { auto v = h::list_snapshots(p.dir); v.push_back(name); return v; }(), keep);
        detail = "would take " + p.dir + "/" + name + " of " + p.subvol +
                 (gone.empty() ? std::string() : "; would expire " + join(gone));
        return true;
    }

    const int sfd = ::open(p.subvol.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sfd < 0) { detail = "can't open " + p.subvol + ": " + std::strerror(errno); return false; }
    if (::mkdirat(sfd, h::kSnapDir, 0755) != 0 && errno != EEXIST) {
        detail = "can't make " + p.dir + ": " + std::strerror(errno);
        ::close(sfd);
        return false;
    }
    const int dfd = ::openat(sfd, h::kSnapDir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat ds{};
    if (dfd < 0 || ::fstat(dfd, &ds) != 0 || !S_ISDIR(ds.st_mode) || ds.st_uid != 0) {
        // A folder someone else made (or a link) is not Rouster's: never snapshot into it.
        detail = p.dir + " is not a folder root owns -- left alone";
        if (dfd >= 0) ::close(dfd);
        ::close(sfd);
        return false;
    }

    btrfs_ioctl_vol_args_v2 a{};
    a.fd = sfd;
    a.flags = BTRFS_SUBVOL_RDONLY;
    std::strncpy(a.name, name.c_str(), BTRFS_SUBVOL_NAME_MAX);
    if (::ioctl(dfd, BTRFS_IOC_SNAP_CREATE_V2, &a) != 0) {
        detail = "the snapshot failed: " + std::string(std::strerror(errno));
        ::close(dfd);
        ::close(sfd);
        return false;
    }
    detail = "took " + p.dir + "/" + name;

    // Keep the newest N of Rouster's own; the one just taken always stays.
    std::vector<std::string> names = h::list_snapshots(p.dir);
    std::vector<std::string> gone, kept_back;
    for (const auto& n : h::to_expire(names, keep)) {
        if (n == name) continue;
        struct stat ns{};
        if (::fstatat(dfd, n.c_str(), &ns, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(ns.st_mode) || ns.st_ino != 256) {
            kept_back.push_back(n + " (not a subvolume)");
            continue;
        }
        btrfs_ioctl_vol_args v{};
        std::strncpy(v.name, n.c_str(), BTRFS_PATH_NAME_MAX);
        if (::ioctl(dfd, BTRFS_IOC_SNAP_DESTROY, &v) == 0) gone.push_back(n);
        else kept_back.push_back(n + " (" + std::strerror(errno) + ")");
    }
    if (!gone.empty()) detail += "; expired " + join(gone);
    if (!kept_back.empty()) detail += "; could not expire " + join(kept_back);
    ::close(dfd);
    ::close(sfd);
    return true;
}

// Run a program (absolute path, fixed argv) and collect its output (stdout +
// stderr, at most 64 KB). Returns the exit code; -1 = could not run.
int run_program(const std::vector<std::string>& argv, std::string& out) {
    int pfd[2];
    if (::pipe2(pfd, O_CLOEXEC) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
    if (pid == 0) {
        const int nul = ::open("/dev/null", O_RDONLY);
        if (nul >= 0) ::dup2(nul, 0);
        ::dup2(pfd[1], 1);
        ::dup2(pfd[1], 2);
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        char* env[] = {const_cast<char*>("PATH=/usr/sbin:/usr/bin"), const_cast<char*>("LC_ALL=C"),
                       const_cast<char*>("HOME=/root"), nullptr};
        ::execve(a[0], a.data(), env);
        ::_exit(127);
    }
    ::close(pfd[1]);
    char buf[4096];
    for (ssize_t n; (n = ::read(pfd[0], buf, sizeof buf)) > 0;)
        if (out.size() < 65536) out.append(buf, static_cast<std::size_t>(n));
    ::close(pfd[0]);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string last_line(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    const auto p = t.rfind('\n');
    return p == std::string::npos ? t : t.substr(p + 1);
}

std::uint64_t pkg_bytes() {
    std::uint64_t b = 0;
    for (const auto& d : h::pkg_cache_dirs()) b += h::du(d).bytes;
    return b;
}

bool pkgclean(bool dry, std::string& detail) {
    const std::string dnf = h::first_program({"/usr/bin/dnf"});
    const std::string apt = dnf.empty() ? h::first_program({"/usr/bin/apt-get"}) : "";
    if (dnf.empty() && apt.empty()) { detail = "neither dnf nor apt-get is here"; return false; }
    const std::vector<std::string> argv = !dnf.empty() ? std::vector<std::string>{dnf, "clean", "all"}
                                                       : std::vector<std::string>{apt, "clean"};
    const std::uint64_t before = pkg_bytes();
    const std::string what = !dnf.empty() ? "dnf clean all" : "apt-get clean";
    if (dry) { detail = "would run " + what + " (" + h::human(before) + " cached)"; return true; }
    std::string out;
    const int code = run_program(argv, out);
    const std::uint64_t after = pkg_bytes();
    if (code != 0) { detail = what + " failed (exit " + std::to_string(code) + "): " + last_line(out); return false; }
    detail = what + " -- freed " + h::human(before > after ? before - after : 0) + " (" + h::human(after) + " left)";
    return true;
}

bool journal(int days, bool dry, std::string& detail) {
    const std::string jc = h::first_program({"/usr/bin/journalctl", "/bin/journalctl"});
    if (jc.empty()) { detail = "journalctl is not here"; return false; }
    const std::uint64_t before = h::du(h::kJournalDir).bytes;
    const std::string arg = "--vacuum-time=" + std::to_string(days) + "d";
    if (dry) { detail = "would run journalctl " + arg + " (" + h::human(before) + " in the journal)"; return true; }
    std::string out;
    const int code = run_program({jc, arg}, out);
    const std::uint64_t after = h::du(h::kJournalDir).bytes;
    if (code != 0) { detail = "journalctl " + arg + " failed (exit " + std::to_string(code) + "): " + last_line(out); return false; }
    detail = "journalctl " + arg + " -- freed " + h::human(before > after ? before - after : 0) + " (" + h::human(after) + " left)";
    return true;
}

bool trim(bool dry, std::string& detail) {
    const std::string ft = h::first_program({"/usr/sbin/fstrim", "/sbin/fstrim", "/usr/bin/fstrim"});
    if (ft.empty()) { detail = "fstrim is not here"; return false; }
    if (dry) { detail = "would run fstrim --all --verbose"; return true; }
    std::string out;
    const int code = run_program({ft, "--all", "--verbose"}, out);
    std::string said;
    std::istringstream in(out);
    for (std::string l; std::getline(in, l);) if (!l.empty()) said += (said.empty() ? "" : "; ") + l;
    if (said.size() > 400) said = said.substr(0, 400) + "...";
    // fstrim --all: exit 0 all trimmed, 32 none supported, 64 some failed (util-linux).
    if (code != 0 && code != 64) { detail = "fstrim failed (exit " + std::to_string(code) + "): " + (said.empty() ? "no output" : said); return false; }
    detail = "fstrim -- " + (said.empty() ? std::string("nothing to trim") : said);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ::umask(022);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "version") { std::printf("rouster-helper 2 (s013b): snapshot pkgclean journal trim\n"); return 0; }
    if (args.empty() || args[0] != "run") return refuse("usage: rouster-helper run [--dry-run] snapshot:N | pkgclean | journal:DAYS | trim");
    if (::geteuid() != 0) return refuse("must run as root, through pkexec");

    // Whose home: pkexec says who asked. Never a path from the command line.
    const char* u = std::getenv("PKEXEC_UID");
    if (!u || !*u || std::strlen(u) > 9 || std::strspn(u, "0123456789") != std::strlen(u))
        return refuse("no PKEXEC_UID -- run through pkexec");
    const uid_t uid = static_cast<uid_t>(std::strtoul(u, nullptr, 10));
    if (uid == 0) return refuse("Rouster runs as a user, not as root");
    const passwd* pw = ::getpwuid(uid);
    if (!pw || !pw->pw_dir || pw->pw_dir[0] != '/') return refuse("no home folder for uid " + std::string(u));
    const std::string home = pw->pw_dir;

    std::size_t i = 1;
    bool dry = false;
    if (i < args.size() && args[i] == "--dry-run") { dry = true; ++i; }
    std::vector<h::Request> reqs;
    for (; i < args.size(); ++i) {
        const auto r = h::parse(args[i]);
        if (!r) return refuse("not a request this helper knows: " + args[i]);
        reqs.push_back(*r);
    }
    if (reqs.empty() || reqs.size() > 8) return refuse("one to eight requests");

    bool all = true;
    for (const auto& r : reqs) {
        std::string detail;
        bool ok = false;
        switch (r.op) {
            case h::Op::Snapshot: ok = snapshot(home, r.arg, dry, detail); break;
            case h::Op::PkgClean: ok = pkgclean(dry, detail); break;
            case h::Op::Journal:  ok = journal(r.arg, dry, detail); break;
            case h::Op::Trim:     ok = trim(dry, detail); break;
        }
        say(h::op_name(r.op), ok ? (dry ? "dry" : "ok") : "failed", detail);
        all = all && ok;
    }
    return all ? 0 : 1;
}
