#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Door -- THE ONE DOOR (s004, REQUIREMENTS §1.1 / §6, RULES "Undo from the
// first write"). Every change Rouster makes to a user's files goes through
// Door::run. Nothing else in the code base renames, unlinks or writes a user
// file, and the selftest's source gate fails if anything does.
//
// The shape (ARCHITECTURE "the plan is data"):
//
//   module -> Plan{ label, actions[], previewed }      (a module never acts)
//   UI shows the plan; the user unchecks what they don't want; previewed=true
//   Door::run(plan):  policy check, every action, BEFORE anything moves
//                     batch line -> for each action:
//                       intent line (fsync) -> the rename -> done / failed line
//   Door::undo():     the last batch, backwards; each item is checked to be
//                     the SAME file (dev + inode) before it is moved back
//
// Two pictures, not an inverse (docs/undo.md): an action records where the
// thing was (from) and where it went (to) plus its identity; undo moves it
// from `to` back to `from`. No per-verb undo code beyond the Trash's info file.
//
// What the door refuses (policy lives HERE, not in modules -- a module bug
// cannot get past it):
//   * a plan that was not previewed;
//   * anything outside home, or home itself;
//   * anything Protected (scan::never_touch(), any fixes/), at any depth;
//   * a path with a symlinked parent (the rename would land somewhere else);
//   * Rouster's own records (the data dir holding the manifest);
//   * Trashing what is already in the Trash; Moving into the Trash;
//   * a Move onto an existing path, or into a missing folder;
//   * crossing filesystems: the rename fails with EXDEV and nothing is copied.
//
// Two verbs CREATE (s007b, retire / restore): Write (a small text file) and
// Clone (git clone). Each builds in a hidden staging name beside the target
// that the door owns, then ONE rename puts it in place -- never half there.
// Their undo is not a rename back (there was nothing before): the created
// thing goes to the Trash, after the same identity check.
//
// One verb MOVES NOTHING (s008, Duplicates): Dedupe shares the blocks of `to`
// with `from` (FIDEDUPERANGE -- the kernel compares the bytes itself, under
// lock, and shares nothing if they differ; a stale hash can't do harm). Both
// paths stay, both contents stay; only free space changes. Its undo puts no
// bytes anywhere: the files are and were identical, so the step is closed
// with nothing to move (re-splitting blocks is not a way back, it's a copy).
//
// Hard-delete does not exist here, and will not (§1.1).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::door {

enum class Verb { Trash, Move, Write, Clone, Dedupe };
const char* verb_name(Verb v);   // "trash", "move", "write", "clone", "dedupe"
bool creates(Verb v);            // Write, Clone: `to` is made; `from` is unused
// THE VERB TABLE the selftest gate walks: a verb added to the enum and not
// here fails the gate's count; every verb here is run through the door,
// checked to be one step, and undone back to the exact tree.
const std::vector<Verb>& verbs();
inline constexpr int kVerbCount = 5;
inline constexpr std::size_t kWriteMax = 64 * 1024;   // Write is for small notes (RESTORE.txt)

struct Action {
    Verb        verb = Verb::Trash;
    std::string from;            // absolute path
    std::string to;              // Move: absolute destination. Trash: filled in by run().
                                 // Write / Clone: the path to create (must not exist).
    std::string reason;          // why the module proposed it (shown in preview)
    std::uint64_t bytes = 0;     // for the preview
    std::string content;         // Write: the file's text (<= kWriteMax)
    std::string url;             // Clone: what to clone
    std::string branch;          // Clone: check this branch out ("" = the remote's default)
    std::string commit;          // Clone: must be this commit or an ancestor of HEAD ("" = any)
    // Dedupe: `from` is the keeper (its blocks), `to` the copy that will share them.
};

struct Plan {
    std::string         label;   // "Sweep: 3 caches" -- names the undo step
    std::vector<Action> actions;
    bool previewed = false;      // the UI sets this after showing the plan (RULES: preview first)
};

struct Identity {
    std::uint64_t dev = 0, ino = 0, size = 0;
    std::int64_t  mtime_ns = 0;
    bool          dir = false;
    bool operator==(const Identity&) const = default;
};

enum class State { Intent, Done, Failed, Undone };

struct Record {
    std::uint64_t id = 0, batch = 0;
    Verb          verb = Verb::Trash;
    std::string   from, to, info;   // info: the .trashinfo path (Trash only)
    Identity      ident;
    std::int64_t  when = 0;         // unix seconds
    State         state = State::Intent;
    std::string   error;
};

struct Batch {
    std::uint64_t id = 0;
    std::string   label;
    std::int64_t  when = 0;
    std::vector<std::uint64_t> records;
    bool          undone = false;
};

struct Result {
    bool ok = false;                       // everything asked for happened
    std::size_t done = 0, failed = 0;
    std::vector<std::string> messages;     // refusals and failures, one per line
    std::uint64_t batch = 0;
};

class Door {
public:
    // home: the user's home (canonicalised). data_dir: where the manifest lives
    // (Rouster's own folder, e.g. ~/.local/share/rouster) -- refused as a target.
    Door(const std::string& home, const std::string& data_dir);

    // Replay the manifest. An intent with no outcome (a crash mid-action) is
    // resolved by looking: the item at `to` with the recorded identity -> done,
    // still at `from` -> failed; either way it is listed in unfinished().
    bool load();

    // "" = allowed; otherwise why not. run() asks this of every action first.
    std::string refusal(const Action& a) const;

    Result run(const Plan& plan);
    Result undo();                          // the last batch not yet undone
    bool        can_undo() const;
    std::string undo_label() const;

    const std::vector<Batch>&  batches() const { return m_batches; }
    const Record*              record(std::uint64_t id) const;
    const std::vector<std::string>& unfinished() const { return m_unfinished; }
    std::string manifest_path() const { return m_manifest; }
    std::string trash_dir() const { return m_trash; }

private:
    bool append(const std::string& line);   // O_APPEND + fdatasync
    bool do_trash(Record& r);
    bool reserve_trash(const std::string& path, std::int64_t when, std::string& dest,
                       std::string& info, std::string& err);   // name + .trashinfo, for `path`
    bool trash_now(const std::string& path, std::string& dest, std::string& err);   // reserve + rename
    bool do_create(Record& r, const Action& a);              // Write / Clone into staging (r.from)
    bool do_move(Record& r);
    bool do_dedupe(Record& r);                               // FIDEDUPERANGE, in chunks
    bool undo_one(Record& r, std::string& why);
    Record* find(std::uint64_t id);

    std::string m_home, m_data, m_manifest, m_trash;
    std::vector<Batch>  m_batches;
    std::vector<Record> m_records;
    std::vector<std::string> m_unfinished;
    std::uint64_t m_next_id = 1, m_next_batch = 1;
};

// Identity of what is at `path` now (lstat; a symlink is itself). false = gone.
bool identity_of(const std::string& path, Identity& out);

// The freedesktop .trashinfo body for `abs` deleted at unix time `when`
// (local time, percent-encoded path). Public so the selftest can check it.
std::string trashinfo(const std::string& abs, std::int64_t when);

}  // namespace rouster::core::door
