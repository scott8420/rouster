#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Preflight (s003, REQUIREMENTS §3) -- the green/red checklist that will gate
// every cleanup once there is a cleanup to gate (s004 on). READ-ONLY: it reads
// /proc/self/mountinfo, statvfs()es home and lstat()s Pika's history file.
//
// Split in two so the selftest can drive every branch without a real machine:
//   gather()   -- the only part that touches the system; returns Inputs.
//   evaluate() -- pure: Inputs -> the checklist.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::preflight {

enum class Status { Green, Amber, Red };
const char* status_name(Status s);

struct Check {
    std::string id;       // "filesystem", "snapshot", "pika", "free" (repos: s006)
    std::string title;    // "Filesystem"
    std::string detail;   // the one line that says why
    Status      status;
};

// One /proc/self/mountinfo line, the fields preflight cares about.
struct Mount {
    std::string mount_point;   // "/home"
    std::string root;          // "/home" -- the subvolume path for btrfs
    std::string fstype;        // "btrfs"
    std::string source;        // "/dev/nvme0n1p6"
};
std::vector<Mount> parse_mountinfo(const std::string& text);
// The mount a path lives on: the longest mount point that is a whole-component
// prefix of it ("/home" holds "/home/scott", not "/homework"). nullptr if none.
const Mount* find_mount(const std::vector<Mount>& mounts, const std::string& path);

struct Inputs {
    std::string                 home;
    std::optional<Mount>        mount;          // the mount holding home
    std::optional<std::int64_t> pika_age_s;     // seconds since Pika's history was written; nullopt = not found
    std::string                 pika_where;     // the file it was read from
    std::uint64_t               free_bytes  = 0;
    std::uint64_t               total_bytes = 0;
    int                         pika_max_days = 7;   // REQUIREMENTS §3.3 default
    // s013: the snapshot row. Read without root: the folder is 0755.
    bool                        snap_possible = false;  // helper::place_for(home).ok
    std::string                 snap_why;               // ...and why not
    std::string                 snap_dir;               // "/home/.rouster-snapshots"
    std::optional<std::int64_t> snap_age_s;             // the newest of Rouster's; nullopt = none
    std::string                 snap_newest;            // its name
    int                         snap_count = 0;
    bool                        helper = false;         // the helper is installed
    int                         snap_fresh_hours = 24;  // newer than this -> green
};

// Where Pika keeps its history, relative to home: the native and the Flatpak
// config dirs. The newest one present wins.
const std::vector<std::string>& pika_history_paths();

Inputs gather(const std::string& home);
std::vector<Check> evaluate(const Inputs& in);

// "3 days", "5 hours", "just now".
std::string human_age(std::int64_t seconds);

}  // namespace rouster::core::preflight
