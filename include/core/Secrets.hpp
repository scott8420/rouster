#pragma once
#include "core/Git.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Secrets (s012c, REQUIREMENTS §4.8, Q11a): ssh keys, gpg, keyrings -- opt-in,
// encrypted with `gpg --symmetric` (AES256), a passphrase typed each time and
// never stored. GTK-free.
//
// Backup: find() LISTS what is under ~/.ssh, ~/.gnupg, ~/.local/share/keyrings
// (regular files and their folders; no links, sockets, lock files). pack()
// reads them at the click, makes a tar IN MEMORY (ustar, written here -- no tar
// child, no plaintext on disk), has gpg encrypt it, then decrypts the result
// once more and checks it gives back the same tar: a backup that can't be
// opened is worse than none. The bytes go to the bundle as ONE door Write
// (secrets.tar.gpg, mode 0600).
//
// Restore: open() decrypts in memory and compares every file with home --
// Same / Missing / Differs / Blocked. Rouster never writes into ~/.ssh,
// ~/.gnupg or the keyrings (§1.5, code not configuration), so what to do comes
// back as lines to run: unpack the missing ones (`tar --skip-old-files`: never
// replaces), and for the ones that differ, unpack beside, into a private
// folder, to compare by hand.
//
// The passphrase goes to gpg on fd 3 (`--passphrase-fd 3`) -- never on argv,
// never in the environment, never logged. gpg runs with its own homedir,
// Rouster's ~/.local/share/rouster/gnupg (so your ~/.gnupg is only READ), with
// --no-symkey-cache, and its agent is stopped after each use.
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::secrets {

inline constexpr const char* kFile = "secrets.tar.gpg";             // in the bundle
inline constexpr std::uint64_t kMax = 3ull * 1024 * 1024;           // the tar; the door writes <= 4 MiB
inline constexpr std::size_t kMinPass = 8;

// The three places, relative to home.
const std::vector<std::string>& roots();

struct Found {
    std::vector<std::string> files;      // relative to home, sorted
    std::vector<std::string> dirs;       // their folders (the roots included), parents first
    std::uint64_t            bytes = 0;
    std::size_t              per_root[3] = {0, 0, 0};
    std::vector<std::string> notes;      // skipped, and why ("~/.ssh/agent.sock -- not a file")
};
Found find(const std::string& home);     // READ-ONLY (lstat only; no file opened)

// ── tar, in memory ──────────────────────────────────────────────────────────
struct Entry {
    std::string   path;                  // relative; a folder ends without '/'
    bool          dir = false;
    std::uint32_t mode = 0600;
    std::int64_t  mtime = 0;
    std::string   bytes;
};
std::string tar(const std::vector<Entry>& entries);
// false (and why) on a bad checksum, an absolute or ".." path, a truncated archive.
bool untar(const std::string& data, std::vector<Entry>& out, std::string& why);

// ── gpg ─────────────────────────────────────────────────────────────────────
// How gpg is run: argv, stdin, the passphrase for fd 3. The selftest can pass
// its own; the real one is vcs::feed.
using Gpg = std::function<vcs::Run(const std::vector<std::string>& argv, const std::string& in, const std::string& pass)>;
Gpg real_gpg(const std::atomic<bool>* cancel = nullptr);

std::string gpg_home(const std::string& home);   // Rouster's own: ~/.local/share/rouster/gnupg

// Typed twice; "" when fine, else why not.
std::string passphrase_problem(const std::string& a, const std::string& b);

struct Packed {
    std::string   data;                  // the encrypted bytes ("" on failure)
    std::size_t   files = 0;
    std::uint64_t raw = 0;               // the tar's size
    std::string   error;
    std::vector<std::string> notes;      // gone since Look, unreadable, ...
};
Packed pack(const std::string& home, const Found& found, const std::string& pass, const Gpg& gpg);

enum class State { Same, Missing, Differs, Blocked };
const char* state_name(State s);

struct Row {
    std::string   rel;
    State         state = State::Same;
    std::uint64_t bytes = 0;
};
struct Command {
    std::string what, line;
};
struct Opened {
    std::vector<Row>     rows;           // files only
    std::vector<Command> commands;
    std::string          error;          // "that passphrase doesn't open it", ...
};
// READ-ONLY: decrypts `<bundle>/secrets.tar.gpg` in memory and compares.
Opened open(const std::string& home, const std::string& bundle, const std::string& pass, const Gpg& gpg);

// Overwrite a string's bytes before it goes (the passphrase, a decrypted tar).
void wipe(std::string& s);

}  // namespace rouster::core::secrets
