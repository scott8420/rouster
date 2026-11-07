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
// It runs no other program and no shell; the snapshot is two btrfs ioctls.
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
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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

}  // namespace

int main(int argc, char** argv) {
    ::umask(022);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "version") { std::printf("rouster-helper 1 (s013a): snapshot\n"); return 0; }
    if (args.empty() || args[0] != "run") return refuse("usage: rouster-helper run [--dry-run] snapshot:N");
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
        }
        say(h::op_name(r.op), ok ? (dry ? "dry" : "ok") : "failed", detail);
        all = all && ok;
    }
    return all ? 0 : 1;
}
