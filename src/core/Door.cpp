#include "core/Door.hpp"
#include "core/Scan.hpp"   // never_touch / classify_dir: the policy is the scan's policy
#include "core/Git.hpp"    // the Clone verb

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <linux/fs.h>        // FIDEDUPERANGE
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// Door.cpp -- the ONLY file in Rouster that changes a user's files. The
// selftest's source gate allowlists it by name; a write call anywhere else in
// src/ or include/ fails the build's selftest.

namespace rouster::core::door {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* verb_name(Verb v) {
    switch (v) {
        case Verb::Trash: return "trash";
        case Verb::Move:  return "move";
        case Verb::Write: return "write";
        case Verb::Clone: return "clone";
        case Verb::Dedupe: return "dedupe";
        case Verb::MakeDir: return "mkdir";
    }
    return "?";
}
bool creates(Verb v) { return v == Verb::Write || v == Verb::Clone || v == Verb::MakeDir; }

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> v = {Verb::Trash, Verb::Move, Verb::Write, Verb::Clone, Verb::Dedupe, Verb::MakeDir};
    return v;
}

namespace {
Verb verb_of(const std::string& s) {
    for (Verb v : verbs()) if (s == verb_name(v)) return v;
    return Verb::Trash;
}
}  // namespace

bool identity_of(const std::string& path, Identity& out) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return false;
    out.dev = st.st_dev;
    out.ino = st.st_ino;
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
    out.dir = S_ISDIR(st.st_mode);
    return true;
}

std::string trashinfo(const std::string& abs, std::int64_t when) {
    std::string enc;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : abs) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') enc += static_cast<char>(c);
        else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
    }
    std::time_t t = static_cast<std::time_t>(when);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    return "[Trash Info]\nPath=" + enc + "\nDeletionDate=" + date + "\n";
}

namespace {

std::int64_t now_s() { return static_cast<std::int64_t>(std::time(nullptr)); }

// rename that never overwrites. renameat2(RENAME_NOREPLACE) where the
// filesystem has it (btrfs, ext4, xfs, tmpfs); else check-then-rename.
int rename_noreplace(const std::string& a, const std::string& b) {
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_NOREPLACE) == 0) return 0;
    if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
    struct stat st{};
    if (::lstat(b.c_str(), &st) == 0) { errno = EEXIST; return -1; }
    return ::rename(a.c_str(), b.c_str());
}

bool inside(const std::string& path, const std::string& dir) {   // strictly below, or equal
    return path == dir || (path.size() > dir.size() && path.rfind(dir, 0) == 0 && path[dir.size()] == '/');
}

std::string ident_json(const Identity& i) {
    return json{{"dev", i.dev}, {"ino", i.ino}, {"size", i.size}, {"mtime_ns", i.mtime_ns}, {"dir", i.dir}}.dump();
}

}  // namespace

Door::Door(const std::string& home, const std::string& data_dir) {
    std::error_code ec;
    m_home = fs::weakly_canonical(home, ec).string();
    m_data = fs::weakly_canonical(data_dir, ec).string();
    m_manifest = m_data + "/manifest.jsonl";
    m_trash = m_home + "/.local/share/Trash";
}

Record* Door::find(std::uint64_t id) {
    for (auto& r : m_records) if (r.id == id) return &r;
    return nullptr;
}
const Record* Door::record(std::uint64_t id) const {
    for (const auto& r : m_records) if (r.id == id) return &r;
    return nullptr;
}

bool Door::append(const std::string& line) {
    std::error_code ec;
    fs::create_directories(m_data, ec);
    const int fd = ::open(m_manifest.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string l = line + "\n";
    const bool ok = ::write(fd, l.data(), l.size()) == static_cast<ssize_t>(l.size()) && ::fdatasync(fd) == 0;
    ::close(fd);
    return ok;
}

bool Door::load() {
    m_batches.clear();
    m_records.clear();
    m_unfinished.clear();
    m_next_id = m_next_batch = 1;
    std::ifstream in(m_manifest);
    if (!in) return true;   // no manifest yet: nothing done, nothing to undo
    std::string line;
    while (std::getline(in, line)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("t")) continue;   // a torn last line from a crash
        const std::string t = j["t"];
        if (t == "batch") {
            Batch b{j.value("batch", 0ull), j.value("label", ""), j.value("when", 0ll), {}, false};
            m_next_batch = std::max(m_next_batch, b.id + 1);
            m_batches.push_back(b);
        } else if (t == "intent") {
            Record r;
            r.id = j.value("id", 0ull);
            r.batch = j.value("batch", 0ull);
            r.verb = verb_of(j.value("verb", ""));
            r.from = j.value("from", "");
            r.to = j.value("to", "");
            r.info = j.value("info", "");
            r.when = j.value("when", 0ll);
            if (j.contains("ident")) {
                const auto& i = j["ident"];
                r.ident = {i.value("dev", 0ull), i.value("ino", 0ull), i.value("size", 0ull),
                           i.value("mtime_ns", 0ll), i.value("dir", false)};
            }
            m_next_id = std::max(m_next_id, r.id + 1);
            m_records.push_back(r);
            for (auto& b : m_batches) if (b.id == r.batch) b.records.push_back(r.id);
        } else if (Record* r = find(j.value("id", 0ull))) {
            if (t == "done")        r->state = State::Done;
            else if (t == "failed") { r->state = State::Failed; r->error = j.value("error", ""); }
            else if (t == "undone") r->state = State::Undone;
            // "undo-failed" leaves it Done: still in the Trash / still moved.
        } else if (t == "batch-undone") {
            for (auto& b : m_batches) if (b.id == j.value("batch", 0ull)) b.undone = true;
        }
    }
    // A crash between intent and outcome: look where the thing is now.
    for (auto& r : m_records) {
        if (r.state != State::Intent) continue;
        Identity at_to{}, at_from{};
        if (identity_of(r.to, at_to) && at_to == r.ident) r.state = State::Done;
        else if (identity_of(r.from, at_from) && at_from == r.ident) r.state = State::Failed;
        else r.state = State::Failed, r.error = "unaccounted for after a crash";
        m_unfinished.push_back(std::string(verb_name(r.verb)) + " " + r.from + " -- " +
                               (r.state == State::Done ? "had happened" : "had not happened"));
    }
    return true;
}

std::string Door::refusal(const Action& a) const { return refusal(a, {}); }

std::string Door::refusal(const Action& a, const std::vector<std::string>& made) const {
    const auto planned = [&](const std::string& p) { return std::find(made.begin(), made.end(), p) != made.end(); };
    const auto bad = [](const std::string& p) {
        return p.empty() || p[0] != '/' || fs::path(p).lexically_normal().string() != p ||
               (p.size() > 1 && p.back() == '/');
    };
    if (!creates(a.verb)) {
        if (bad(a.from)) return "not a clean absolute path: " + a.from;
        if (!inside(a.from, m_home) || a.from == m_home) return "outside the home folder: " + a.from;
    }

    // Every component from home down: no symlinked parent, nothing Protected.
    const auto walk = [&](const std::string& p, bool last_may_be_missing) -> std::string {
        const std::string rel = p.substr(m_home.size() + 1);
        std::string cur;
        scan::Kind kind = scan::Kind::Other;
        std::size_t pos = 0;
        while (true) {
            const auto s = rel.find('/', pos);
            cur = rel.substr(0, s);
            const bool last = s == std::string::npos;
            kind = scan::classify_dir(cur, kind, false).kind;
            if (kind == scan::Kind::Protected) return "protected (never touched): ~/" + cur;
            struct stat st{};
            const std::string abs = m_home + "/" + cur;
            if (::lstat(abs.c_str(), &st) != 0) {
                if (last && last_may_be_missing) break;
                if (planned(abs)) { if (last) break; pos = s + 1; continue; }   // an earlier MakeDir makes it
                return "does not exist: " + abs;
            }
            if (!last && S_ISLNK(st.st_mode)) return "goes through a symlink: ~/" + cur;
            if (last) break;
            pos = s + 1;
        }
        return {};
    };
    if (creates(a.verb)) {
        if (!a.from.empty()) return "a " + std::string(verb_name(a.verb)) + " has no source";
        if (bad(a.to)) return "not a clean absolute path to create: " + a.to;
        if (!inside(a.to, m_home) || a.to == m_home) return "outside the home folder: " + a.to;
        if (inside(a.to, m_trash) || inside(m_trash, a.to)) return "not in the Trash: " + a.to;
        if (inside(a.to, m_data) || inside(m_data, a.to)) return "Rouster's own records: " + a.to;
        if (auto w = walk(a.to, true); !w.empty()) return w;
        struct stat st{};
        if (::lstat(a.to.c_str(), &st) == 0 || planned(a.to)) return "already exists: " + a.to;
        const std::string parent = fs::path(a.to).parent_path().string();
        if (!planned(parent) && (::stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return "folder missing: " + parent;
        if (a.verb == Verb::MakeDir) return {};
        if (a.verb == Verb::Write) {
            if (a.content.size() > kWriteMax) return "too big to write: " + a.to;
            return {};
        }
        const auto flag = [](const std::string& s) { return !s.empty() && s[0] == '-'; };
        if (a.url.empty() || flag(a.url)) return "no clone URL for " + a.to;
        if (flag(a.branch)) return "not a branch name: " + a.branch;
        for (char c : a.commit)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return "not a commit: " + a.commit;
        return {};
    }
    if (auto w = walk(a.from, false); !w.empty()) return w;
    if (inside(a.from, m_data) || inside(m_data, a.from)) return "Rouster's own records: " + a.from;

    if (a.verb == Verb::Dedupe) {
        if (bad(a.to)) return "not a clean absolute path: " + a.to;
        if (!inside(a.to, m_home) || a.to == m_home) return "outside the home folder: " + a.to;
        if (auto w = walk(a.to, false); !w.empty()) return w;
        for (const auto* p : {&a.from, &a.to}) {
            if (inside(*p, m_trash)) return "in the Trash: " + *p;
            if (inside(*p, m_data)) return "Rouster's own records: " + *p;
        }
        struct stat sa{}, sb{};
        if (::lstat(a.from.c_str(), &sa) != 0 || !S_ISREG(sa.st_mode)) return "not a file: " + a.from;
        if (::lstat(a.to.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return "not a file: " + a.to;
        if (sa.st_dev != sb.st_dev) return "on different filesystems: " + a.to;
        if (sa.st_ino == sb.st_ino) return "the same file (a hard link): " + a.to;
        if (sa.st_size != sb.st_size) return "not the same size: " + a.to;
        return {};
    }
    if (a.verb == Verb::Trash) {
        if (inside(a.from, m_trash) || inside(m_trash, a.from)) return "already in the Trash: " + a.from;
        return {};
    }
    // Move.
    if (bad(a.to)) return "not a clean absolute destination: " + a.to;
    if (!inside(a.to, m_home) || a.to == m_home) return "destination outside the home folder: " + a.to;
    if (inside(a.to, a.from)) return "cannot move a folder into itself: " + a.to;
    if (inside(a.to, m_trash)) return "use Trash, not Move, for the Trash: " + a.to;
    if (inside(a.to, m_data)) return "Rouster's own records: " + a.to;
    if (auto w = walk(a.to, true); !w.empty()) return "destination " + w;
    struct stat st{};
    if (::lstat(a.to.c_str(), &st) == 0 || planned(a.to)) return "destination exists: " + a.to;
    const std::string parent = fs::path(a.to).parent_path().string();
    if (!planned(parent) && (::stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
        return "destination folder missing: " + parent;
    return {};
}

bool Door::reserve_trash(const std::string& path, std::int64_t when, std::string& dest_out,
                         std::string& info_out, std::string& err) {
    std::error_code ec;
    fs::create_directories(m_trash + "/files", ec);
    fs::create_directories(m_trash + "/info", ec);
    const std::string base = fs::path(path).filename().string();
    // Reserve a name by creating its .trashinfo exclusively (the spec's way).
    for (int n = 1; n < 10000; ++n) {
        const std::string name = n == 1 ? base : base + "." + std::to_string(n);
        const std::string info = m_trash + "/info/" + name + ".trashinfo";
        const std::string dest = m_trash + "/files/" + name;
        struct stat st{};
        if (::lstat(dest.c_str(), &st) == 0) continue;
        const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; err = std::strerror(errno); return false; }
        const std::string body = trashinfo(path, when);
        const bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!wrote) { ::unlink(info.c_str()); err = "could not write the trash info"; return false; }
        dest_out = dest;
        info_out = info;
        return true;
    }
    err = "no free name in the Trash";
    return false;
}

// Share `to`'s blocks with `from`'s. The kernel locks both, compares the bytes,
// and shares only what is equal -- if anything differs it reports DIFFERS and
// that range is left exactly as it was. Opened read-only: the owner may dedupe.
bool Door::do_dedupe(Record& r) {
    const int src = ::open(r.from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int dst = ::open(r.to.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const auto close_both = [&] { if (src >= 0) ::close(src); if (dst >= 0) ::close(dst); };
    if (src < 0 || dst < 0) { r.error = std::strerror(errno); close_both(); return false; }
    struct stat st{};
    ::fstat(src, &st);
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    constexpr std::uint64_t kChunk = 16ull << 20;   // btrfs takes up to 16 MiB per call
    std::vector<char> buf(sizeof(file_dedupe_range) + sizeof(file_dedupe_range_info));
    auto* range = reinterpret_cast<file_dedupe_range*>(buf.data());
    for (std::uint64_t off = 0; off < size;) {
        std::memset(buf.data(), 0, buf.size());
        range->src_offset = off;
        range->src_length = std::min(kChunk, size - off);
        range->dest_count = 1;
        range->info[0].dest_fd = dst;
        range->info[0].dest_offset = off;
        if (::ioctl(src, FIDEDUPERANGE, range) != 0) {
            r.error = errno == EOPNOTSUPP || errno == EINVAL || errno == ENOTTY
                          ? "this filesystem can't share blocks" : std::string(std::strerror(errno));
            close_both();
            return false;
        }
        if (range->info[0].status == FILE_DEDUPE_RANGE_DIFFERS) {
            r.error = "the contents differ now -- nothing more was shared";
            close_both();
            return false;
        }
        if (range->info[0].status < 0) {
            r.error = std::strerror(-range->info[0].status);
            close_both();
            return false;
        }
        if (range->info[0].bytes_deduped == 0) { r.error = "the filesystem shared nothing"; close_both(); return false; }
        off += range->info[0].bytes_deduped;
    }
    close_both();
    return true;
}

bool Door::do_trash(Record& r) {
    return reserve_trash(r.from, r.when, r.to, r.info, r.error);
}

bool Door::trash_now(const std::string& path, std::string& dest, std::string& err) {
    std::string info;
    if (!reserve_trash(path, now_s(), dest, info, err)) return false;
    if (rename_noreplace(path, dest) != 0) {
        err = std::strerror(errno);
        ::unlink(info.c_str());
        return false;
    }
    return true;
}

// Write / Clone build in a staging name the door owns -- `.rouster-<name>.<id>`
// beside the target -- recorded as r.from. run() then renames it into place.
// A staging clone that fails goes to the Trash, never rm -r (§1.1).
bool Door::do_create(Record& r, const Action& a) {
    const fs::path to(r.to);
    r.from = (to.parent_path() / (".rouster-" + to.filename().string() + "." + std::to_string(r.id))).string();
    struct stat st{};
    if (::lstat(r.from.c_str(), &st) == 0) { r.error = "staging name taken: " + r.from; return false; }
    if (r.verb == Verb::MakeDir) {
        if (::mkdir(r.from.c_str(), 0755) != 0) { r.error = std::strerror(errno); return false; }
    } else if (r.verb == Verb::Write) {
        const int fd = ::open(r.from.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) { r.error = std::strerror(errno); return false; }
        const bool wrote = ::write(fd, a.content.data(), a.content.size()) == static_cast<ssize_t>(a.content.size()) &&
                           ::fsync(fd) == 0;
        ::close(fd);
        if (!wrote) { r.error = "could not write " + r.from; ::unlink(r.from.c_str()); return false; }
    } else {
        const std::string parent = to.parent_path().string();
        const auto fail = [&](const std::string& why) {
            r.error = why;
            std::string dest, err;
            if (::lstat(r.from.c_str(), &st) == 0) trash_now(r.from, dest, err);
            return false;
        };
        const auto c = vcs::git(parent, {"clone", "-q", "--", a.url, r.from}, 900);
        if (c.code != 0)
            return fail("git clone failed: " + (c.timed_out ? std::string("no answer in 15 minutes") : c.err.substr(0, c.err.find('\n'))));
        if (!a.branch.empty()) {
            const auto cur = vcs::git(r.from, {"symbolic-ref", "-q", "--short", "HEAD"}, 20);
            if (cur.out.substr(0, cur.out.find('\n')) != a.branch &&
                vcs::git(r.from, {"checkout", "-q", a.branch}, 60).code != 0)
                return fail("the clone has no branch " + a.branch);
        }
        if (!a.commit.empty() && vcs::git(r.from, {"merge-base", "--is-ancestor", a.commit, "HEAD"}, 20).code != 0)
            return fail("commit " + a.commit.substr(0, 7) + " is not in the clone's history");
    }
    if (!identity_of(r.from, r.ident)) { r.error = "staging vanished: " + r.from; return false; }
    return true;
}

Result Door::run(const Plan& plan) {
    Result res;
    if (!plan.previewed) { res.messages.push_back("refused: the plan was never previewed"); return res; }
    if (plan.actions.empty()) { res.ok = true; return res; }

    // Policy first, for the WHOLE plan, before anything moves.
    {
        std::vector<std::string> made;
        for (const auto& a : plan.actions) {
            if (auto why = refusal(a, made); !why.empty()) res.messages.push_back("refused: " + why);
            if (a.verb == Verb::MakeDir) made.push_back(a.to);
        }
    }
    if (!res.messages.empty()) return res;

    Batch b{m_next_batch++, plan.label, now_s(), {}, false};
    if (!append(json{{"t", "batch"}, {"batch", b.id}, {"label", b.label}, {"when", b.when}}.dump())) {
        res.messages.push_back("could not write the manifest " + m_manifest + " -- nothing done");
        return res;
    }
    m_batches.push_back(b);
    Batch& batch = m_batches.back();
    res.batch = batch.id;

    for (const auto& a : plan.actions) {
        Record r;
        r.id = m_next_id++;
        r.batch = batch.id;
        r.verb = a.verb;
        r.from = a.from;
        r.to = a.to;
        r.when = now_s();
        if (creates(r.verb)) { if (!do_create(r, a)) { /* r.error set */ } }
        else if (r.verb == Verb::Dedupe) { if (!identity_of(r.to, r.ident)) r.error = "vanished before it could be shared"; }
        else if (!identity_of(r.from, r.ident)) { r.error = "vanished before it could be moved"; }
        else if (r.verb == Verb::Trash && !do_trash(r)) { /* r.error set */ }

        if (r.error.empty()) {
            // Write-ahead: the intent is on disk before the rename happens.
            if (!append(json{{"t", "intent"}, {"id", r.id}, {"batch", r.batch}, {"verb", verb_name(r.verb)},
                             {"from", r.from}, {"to", r.to}, {"info", r.info}, {"when", r.when},
                             {"ident", json::parse(ident_json(r.ident))}}.dump())) {
                if (!r.info.empty()) ::unlink(r.info.c_str());
                res.messages.push_back("could not write the manifest -- stopped before " + r.from);
                break;
            }
            m_records.push_back(r);
            batch.records.push_back(r.id);
            Record& live = m_records.back();
            const bool acted = live.verb == Verb::Dedupe ? do_dedupe(live) : rename_noreplace(live.from, live.to) == 0;
            if (acted) {
                live.state = State::Done;
                append(json{{"t", "done"}, {"id", live.id}}.dump());
                ++res.done;
            } else {
                if (live.verb != Verb::Dedupe)
                    live.error = errno == EXDEV ? "on another filesystem -- Rouster never copies-and-deletes"
                                                : std::string(std::strerror(errno));
                if (!live.info.empty()) ::unlink(live.info.c_str());
                if (creates(live.verb)) { std::string d, e; trash_now(live.from, d, e); }   // the staging copy
                live.state = State::Failed;
                append(json{{"t", "failed"}, {"id", live.id}, {"error", live.error}}.dump());
                ++res.failed;
                res.messages.push_back("failed: " + (creates(live.verb) || live.verb == Verb::Dedupe ? live.to : live.from) +
                                       " -- " + live.error);
            }
        } else {
            ++res.failed;
            res.messages.push_back("failed: " + (creates(r.verb) ? r.to : r.from) + " -- " + r.error);
        }
    }
    res.ok = res.failed == 0 && res.done == plan.actions.size();
    return res;
}

bool Door::undo_one(Record& r, std::string& why) {
    Identity now{};
    if (!identity_of(r.to, now)) { why = "no longer at " + r.to; return false; }
    if (r.verb == Verb::Dedupe) {   // nothing moved: the copy must still be the copy, then the step is closed
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino)) { why = "something else is at " + r.to + " now"; return false; }
        return true;
    }
    if (creates(r.verb)) {   // nothing was there before: what was made goes to the Trash
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino && now.dir == r.ident.dir)) {
            why = "something else is at " + r.to + " now";
            return false;
        }
        if (r.verb == Verb::MakeDir) {   // only an empty folder goes; anything put in it since stays put
            std::error_code ec;
            if (!fs::is_empty(r.to, ec) || ec) { why = "something is in " + r.to + " now -- left in place"; return false; }
        }
        std::string dest;
        if (!trash_now(r.to, dest, why)) return false;
        r.info = dest;
        return true;
    }
    if (!(now == r.ident)) {
        // A directory's mtime/size change when something inside it changes;
        // what must match is that it is the same object.
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino && now.dir == r.ident.dir)) {
            why = "something else is at " + r.to + " now";
            return false;
        }
    }
    struct stat st{};
    const std::string parent = fs::path(r.from).parent_path().string();
    if (::stat(parent.c_str(), &st) != 0) { why = "its folder is gone: " + parent; return false; }
    if (rename_noreplace(r.to, r.from) != 0) {
        why = errno == EEXIST ? "something new is at " + r.from : std::string(std::strerror(errno));
        return false;
    }
    if (!r.info.empty()) ::unlink(r.info.c_str());
    return true;
}

bool Door::can_undo() const {
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) return true;
    return false;
}

std::string Door::undo_label() const {
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) return it->label;
    return {};
}

Result Door::undo() {
    Result res;
    Batch* b = nullptr;
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) { b = &*it; break; }
    if (!b) { res.messages.push_back("nothing to undo"); return res; }
    res.batch = b->id;
    std::size_t todo = 0;
    for (auto it = b->records.rbegin(); it != b->records.rend(); ++it) {
        Record* r = find(*it);
        if (!r || r->state != State::Done) continue;
        ++todo;
        append(json{{"t", "undo-intent"}, {"id", r->id}}.dump());
        std::string why;
        if (undo_one(*r, why)) {
            r->state = State::Undone;
            append(json{{"t", "undone"}, {"id", r->id}}.dump());
            ++res.done;
        } else {
            append(json{{"t", "undo-failed"}, {"id", r->id}, {"error", why}}.dump());
            ++res.failed;
            res.messages.push_back("could not put back " + r->from + " -- " + why);
        }
    }
    // The batch is closed even if an item could not go back: the failure is
    // in the manifest and the message; retrying blindly would not fix it.
    b->undone = true;
    append(json{{"t", "batch-undone"}, {"batch", b->id}}.dump());
    res.ok = res.failed == 0 && res.done == todo;
    return res;
}

}  // namespace rouster::core::door
