#pragma once
#include "core/Door.hpp"
#include "core/Git.hpp"
#include "core/Secrets.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Profile backup (s012a, REQUIREMENTS §4.8). GTK-free, READ-ONLY; the door
// acts. A snapshot of *the user experience*, not a full backup.
//
// find() gathers, reading only:
//   Dotfiles   -- a fixed list (.bashrc, .gitconfig, .ssh/config, ...), those there.
//   GNOME      -- `dconf dump /` (the text; the dconf database itself is never
//                 read or touched), and the extensions in
//                 ~/.local/share/gnome-shell/extensions as a LIST (uuid, name,
//                 version) -- that folder is Protected, its files stay put.
//   Autostart  -- ~/.config/autostart/*.
//   User units -- ~/.config/systemd/user: the unit files, and which are
//                 enabled (the *.wants links, as a list).
//   Fonts      -- ~/.local/share/fonts, every file.
//   Packages   -- `dnf -C repoquery --userinstalled` and `flatpak list --app`
//                 (cache only: Look never touches the network).
//   Notes      -- free-form, for what can't be packaged (Rouster's own file).
// Secrets (ssh keys, gpg, keyrings) are NOT here: s012b, opt-in and encrypted.
//
// plan() builds ONE door step that CREATES a bundle folder and nothing else:
//   ~/Rouster/Profile/<host>-<yyyymmdd-hhmm>/   (s019d: Rouster's own name -- ~/Archive is Tidy's)
//     PROFILE.txt         what is in it, from where, how to put it back by hand
//     NOTES.md            the notes (if any)
//     home/<rel>          files, at their place relative to home
//     gnome/dconf.ini     gnome/extensions.txt
//     units/enabled.txt   packages/dnf.txt   packages/flatpak.txt
// Files are Copied (reflink where it can); a file under a Protected folder
// (.ssh/config) is read here and Written -- the door never copies out of a
// Protected folder. Undo sends the whole bundle to the Trash.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::profile {

enum class Group { Dotfiles, Gnome, Autostart, Units, Fonts, Packages };
inline constexpr int kGroupCount = 6;
const char* group_name(Group g);
const char* group_why(Group g);

inline constexpr const char* kRoot = "Rouster/Profile";   // relative to home; new backups go here (s019d)
inline constexpr const char* kOldRoot = "Archive/Profile"; // before s019d: still listed and restorable, never written
inline constexpr std::uint64_t kFileMax = 64ull * 1024 * 1024;   // a bigger "dotfile" is left out, said why

struct Item {
    Group         group = Group::Dotfiles;
    std::string   rel;               // the source, relative to home ("" = made here: dconf, lists)
    std::string   dest;              // inside the bundle: "home/.bashrc", "gnome/dconf.ini"
    std::string   label;             // what the row shows
    bool          made = false;      // content was produced by Rouster (Write)
    bool          read = false;      // a file under a Protected folder: content read, Written
    std::string   content;           // made / read
    std::uint64_t bytes = 0;
    std::uint64_t dev = 0, ino = 0;  // re-checked at plan time (files only)
    std::int64_t  mtime_ns = 0;
    bool          checked = true;
    std::string   note;              // shown beside the row
};

struct Found {
    std::vector<Item>        items;  // by group, then path
    std::vector<std::string> notes;  // what couldn't be gathered, and why ("dnf: not installed")
    std::string              host;
    std::uint64_t            bytes = 0;
    std::uint64_t            errors = 0;
    std::vector<std::string> bundles;   // earlier bundles, newest first (relative to home)
    std::string              refused;
    secrets::Found           secrets;   // s012c: listed only; read and encrypted at the click, if opted in
};

// How find() runs a program (the selftest passes a fake). Default: vcs::child.
using Runner = std::function<vcs::Run(const std::vector<std::string>& argv)>;
Runner real_runner(const std::atomic<bool>* cancel = nullptr);
// s012d: with a fixture home (ROUSTER_HOME), dconf reads the fixture's own
// database (door::fixture_dconf_argv); everything else runs as real_runner.
Runner fixture_runner(const std::string& home, const std::atomic<bool>* cancel = nullptr);

// The fixed dotfile list, relative to home.
const std::vector<std::string>& dotfiles();

// "fedora-mbp", 2026-11-06 15:55 -> "fedora-mbp-20261106-1555"
std::string bundle_name(const std::string& host, std::int64_t now);

// READ-ONLY.
Found find(const std::string& home, const Runner& run);

// The checked items -> one plan: the missing folders (parents first), the
// bundle and its subfolders, a Copy / Write per item, NOTES.md, PROFILE.txt.
// Each file is looked at again first; anything changed or gone is left out
// and named in `dropped`. A bundle name already taken gets -2, -3.
// `bundle` is set to the bundle's absolute path. previewed stays false.
// s012c: `secrets` -- the encrypted bytes from secrets::pack() ("" = not
// opted in) -- becomes one more Write, secrets.tar.gpg, mode 0600.
door::Plan plan(const std::string& home, const Found& found, const std::string& notes,
                std::int64_t now, std::string& bundle, std::vector<std::string>& dropped,
                const std::string& secrets = {}, std::size_t secret_files = 0);

// ── Restore (s012b) ─────────────────────────────────────────────────────────
// READ-ONLY look at a bundle against what is in home now. Every file under the
// bundle's home/ is one row:
//   Same      -- identical bytes: nothing to do;
//   Missing   -- not there: offered, pre-checked (Copy in);
//   Differs   -- there, different: offered UNCHECKED (§1.6), with `diff -u`
//                for text; restoring it sends the one there to the Trash and
//                Copies the backup's in -- one step, Undo puts it back;
//   Protected -- under a never-touch folder (~/.ssh): never written; the
//                command to do it yourself is listed instead;
//   Blocked   -- a folder or a link where the file would go: not offered.
// What Rouster doesn't do itself comes back as commands for you to run:
// missing dnf / flatpak packages, enabling user services, fc-cache, the
// extensions to fetch. GNOME settings are a door step since s012d.
enum class State { Same, Missing, Differs, Protected, Blocked };
const char* state_name(State s);

struct Back {
    std::string   rel;               // relative to home
    std::string   from;              // the bundle's file (absolute)
    State         state = State::Same;
    std::uint64_t bytes = 0;         // the bundle's file
    std::string   diff;              // Differs: `diff -u now backup` (text), or why there is none
    bool          checked = false;
    std::uint64_t ino = 0;           // what is there now (Differs), re-checked at plan time
    std::int64_t  mtime_ns = 0;
    std::int64_t  from_mtime_ns = 0; // the bundle's file, re-checked too
};

struct Command {
    std::string what;                // "dnf: 3 packages not installed"
    std::string line;                // the shell line, ready to paste
};

// GNOME settings (s012d): the bundle's dconf dump against `dconf dump /` now,
// key by key. Offered UNCHECKED (it changes your live set-up); restoring is one
// door Settings step -- the current settings are saved first, Undo puts them
// back exactly. Keys you set since that the backup doesn't name stay.
struct SettingsBack {
    bool          there = false;     // the bundle has gnome/dconf.ini
    bool          differs = false;   // loading it would change something
    bool          checked = false;
    std::size_t   changes = 0;       // keys the load would set to another value (or set at all)
    std::size_t   kept = 0;          // keys set now that the backup doesn't name (left as they are)
    std::string   what;              // the changes, one key per block: path / now / backup
    std::string   from;              // the bundle's dconf.ini
    std::int64_t  from_mtime_ns = 0;
    std::string   error;             // dconf couldn't be read now
};

struct Restore {
    std::string              bundle; // absolute
    std::string              name;   // its folder name
    std::string              host, made;   // from PROFILE.txt
    std::vector<Back>        files;
    std::vector<Command>     commands;
    std::vector<std::string> notes;
    std::string              refused;
    std::uint64_t            secrets = 0;   // s012c: size of secrets.tar.gpg in the bundle (0 = none)
    SettingsBack             settings;      // s012d
};

// Earlier bundles (those holding PROFILE.txt) under ~/Rouster/Profile and the old
// ~/Archive/Profile, newest first -- each relative to home ("Rouster/Profile/<name>").
std::vector<std::string> bundles(const std::string& home);

Restore look_back(const std::string& home, const std::string& bundle, const Runner& run);

// The checked Missing / Differs rows: MakeDir for missing folders (parents
// first), then per file [Trash what is there] + Copy from the bundle; then,
// if checked, ONE Settings action loading the bundle's dconf.ini (s012d). Each row
// is looked at again first; anything changed since the look is left out and
// named. previewed stays false.
door::Plan restore_plan(const std::string& home, const Restore& r, std::vector<std::string>& dropped);

// The PROFILE.txt text for these items (public so the selftest can read it).
std::string profile_txt(const Found& found, const std::vector<const Item*>& items, std::int64_t now,
                        const std::string& notes, std::size_t secret_files = 0);

}  // namespace rouster::core::profile
