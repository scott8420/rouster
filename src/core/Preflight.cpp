#include "core/Preflight.hpp"
#include "core/Scan.hpp"   // human_bytes

#include <ctime>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <sys/statvfs.h>

// Preflight.cpp -- read-only. gather() reads; evaluate() is pure.

namespace rouster::core::preflight {

const char* status_name(Status s) {
    switch (s) {
        case Status::Green: return "green";
        case Status::Amber: return "amber";
        case Status::Red:   return "red";
    }
    return "?";
}

namespace {
// mountinfo escapes space, tab, newline and backslash as \ooo.
std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i + 1] >= '0' && s[i + 1] <= '7' && s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            out += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}
}  // namespace

std::vector<Mount> parse_mountinfo(const std::string& text) {
    // id parent maj:min root mount_point options [optional...] - fstype source superopts
    std::vector<Mount> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream f(line);
        std::vector<std::string> w;
        for (std::string t; f >> t;) w.push_back(t);
        std::size_t dash = 0;
        for (std::size_t i = 6; i < w.size(); ++i)
            if (w[i] == "-") { dash = i; break; }
        if (w.size() < 7 || dash == 0 || dash + 1 >= w.size()) continue;
        Mount m;
        m.root        = unescape(w[3]);
        m.mount_point = unescape(w[4]);
        m.fstype      = w[dash + 1];
        m.source      = dash + 2 < w.size() ? unescape(w[dash + 2]) : std::string{};
        out.push_back(std::move(m));
    }
    return out;
}

const Mount* find_mount(const std::vector<Mount>& mounts, const std::string& path) {
    const Mount* best = nullptr;
    for (const auto& m : mounts) {
        const std::string& mp = m.mount_point;
        const bool holds = mp == "/" ||
            (path.rfind(mp, 0) == 0 && (path.size() == mp.size() || path[mp.size()] == '/'));
        // Later lines win a tie: a mount over a mount hides the one beneath.
        if (holds && (!best || mp.size() >= best->mount_point.size())) best = &m;
    }
    return best;
}

const std::vector<std::string>& pika_history_paths() {
    static const std::vector<std::string> p = {
        ".config/pika-backup/history.json",
        ".var/app/org.gnome.World.PikaBackup/config/pika-backup/history.json",
    };
    return p;
}

Inputs gather(const std::string& home) {
    Inputs in;
    in.home = home;

    std::ifstream mi("/proc/self/mountinfo");
    std::stringstream buf;
    buf << mi.rdbuf();
    const auto mounts = parse_mountinfo(buf.str());
    if (const Mount* m = find_mount(mounts, home)) in.mount = *m;

    const std::time_t now = std::time(nullptr);
    for (const auto& rel : pika_history_paths()) {
        struct stat st{};
        const std::string p = home + "/" + rel;
        if (::lstat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        const std::int64_t age = static_cast<std::int64_t>(now - st.st_mtime);
        if (!in.pika_age_s || age < *in.pika_age_s) { in.pika_age_s = age; in.pika_where = p; }
    }

    struct statvfs vs{};
    if (::statvfs(home.c_str(), &vs) == 0) {
        in.free_bytes  = static_cast<std::uint64_t>(vs.f_bavail) * vs.f_frsize;
        in.total_bytes = static_cast<std::uint64_t>(vs.f_blocks) * vs.f_frsize;
    }
    return in;
}

std::string human_age(std::int64_t s) {
    if (s < 0) s = 0;
    const auto plural = [](std::int64_t n, const char* u) {
        return std::to_string(n) + " " + u + (n == 1 ? "" : "s");
    };
    if (s < 120)          return "just now";
    if (s < 2 * 3600)     return plural(s / 60, "minute");
    if (s < 2 * 86400)    return plural(s / 3600, "hour");
    return plural(s / 86400, "day");
}

std::vector<Check> evaluate(const Inputs& in) {
    std::vector<Check> out;
    const bool btrfs = in.mount && in.mount->fstype == "btrfs";
    const bool own   = btrfs && in.mount->mount_point != "/";

    // 1. Filesystem.
    {
        Check c{"filesystem", "Filesystem", "", Status::Amber};
        if (!in.mount) {
            c.detail = "Could not tell which filesystem holds " + in.home + ".";
        } else if (own) {
            c.status = Status::Green;
            c.detail = "btrfs, its own subvolume (" + in.mount->source + "[" + in.mount->root + "] on " +
                       in.mount->mount_point + ") -- it can be snapshotted on its own.";
        } else if (btrfs) {
            c.detail = "btrfs, but home shares the system's subvolume (" + in.mount->root +
                       ") -- a snapshot would take the whole system with it.";
        } else {
            c.detail = in.mount->fstype + " on " + in.mount->mount_point +
                       " -- no snapshots here; a recent Pika backup is the safety net.";
        }
        out.push_back(c);
    }

    // 2. Snapshot. Check-only at s003 (Q3): taking one needs the root helper.
    {
        Check c{"snapshot", "Snapshot", "", Status::Amber};
        c.detail = btrfs
            ? "Not taken yet -- snapshots need the small root helper, due before the first clean (s005)."
            : "Not available on " + (in.mount ? in.mount->fstype : std::string("this filesystem")) + ".";
        out.push_back(c);
    }

    // 3. Pika. Without snapshots it is the only net, so stale goes red.
    {
        Check c{"pika", "Pika Backup", "", Status::Green};
        const Status stale = btrfs ? Status::Amber : Status::Red;
        if (!in.pika_age_s) {
            c.status = stale;
            c.detail = "No Pika Backup history found -- is Pika set up for this account?";
        } else if (*in.pika_age_s > static_cast<std::int64_t>(in.pika_max_days) * 86400) {
            c.status = stale;
            c.detail = "Last backup " + human_age(*in.pika_age_s) + " ago -- older than " +
                       std::to_string(in.pika_max_days) + " days.";
        } else {
            c.detail = "Last backup " + human_age(*in.pika_age_s) + " ago.";
        }
        out.push_back(c);
    }

    // 4. Free space: room for a snapshot's delta and archive moves.
    {
        Check c{"free", "Free space", "", Status::Green};
        constexpr std::uint64_t GB = 1000ull * 1000 * 1000;
        const std::uint64_t pct = in.total_bytes ? in.free_bytes * 100 / in.total_bytes : 0;
        c.detail = scan::human_bytes(in.free_bytes) + " free of " + scan::human_bytes(in.total_bytes) +
                   " (" + std::to_string(pct) + "%).";
        if (in.total_bytes == 0)                                  { c.status = Status::Red;   c.detail = "Could not read free space."; }
        else if (in.free_bytes < 2 * GB)                          c.status = Status::Red;
        else if (in.free_bytes < 10 * GB || pct < 10)             c.status = Status::Amber;
        out.push_back(c);
    }

    // 5. Repos are checked per project when one is retired (s006) -- no row
    // until then: a green light for a check that does not run would be a lie.
    return out;
}

}  // namespace rouster::core::preflight
