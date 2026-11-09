#include "core/Catalog.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

// Catalog.cpp -- the only file that opens a database (the source gate holds
// `sqlite3_open` to it). It writes exactly one thing: <data_dir>/catalog.db,
// Rouster's own memory -- never a file of yours. Setting a broken catalog aside
// is a rename inside the data folder; nothing is ever deleted here.

namespace rouster::core::catalog {

namespace {

namespace fs = std::filesystem;
constexpr int kAppId = 0x524F5553;   // "ROUS" -- PRAGMA application_id: this database is Rouster's

const char* kSchemaSql = R"SQL(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE looks (
    id INTEGER PRIMARY KEY, at INTEGER NOT NULL, root TEXT NOT NULL, atime_mode TEXT,
    files INTEGER, fresh INTEGER, changed INTEGER, read INTEGER, moved INTEGER, gone INTEGER, returned INTEGER);
CREATE TABLE files (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,              -- relative to home
    package INTEGER NOT NULL DEFAULT 0,     -- a Mac package, ONE row (s015a)
    ext TEXT, kind TEXT,                    -- kind: s015c (GIO content type)
    size INTEGER, first_seen INTEGER NOT NULL, born INTEGER,
    last_read INTEGER, last_changed INTEGER,
    dev INTEGER, ino INTEGER, seen_look INTEGER, gone INTEGER);
CREATE INDEX files_ino ON files(dev, ino);
CREATE TABLE history (file_id INTEGER NOT NULL, look_id INTEGER NOT NULL,
    last_read INTEGER, last_changed INTEGER, size INTEGER);
CREATE INDEX history_file ON history(file_id);
CREATE TABLE moves (file_id INTEGER NOT NULL, look_id INTEGER NOT NULL, from_path TEXT, to_path TEXT);
CREATE INDEX moves_file ON moves(file_id);
CREATE TABLE decisions (id INTEGER PRIMARY KEY, path TEXT NOT NULL, file_id INTEGER,
    verdict TEXT NOT NULL, detail TEXT, made_at INTEGER NOT NULL);
CREATE INDEX decisions_path ON decisions(path);
)SQL";

// ── A thin RAII layer over sqlite3 ────────────────────────────────────────────
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    explicit operator bool() const { return s != nullptr; }
    Stmt& bind(int i, std::int64_t v) { sqlite3_bind_int64(s, i, v); return *this; }
    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); return *this; }
    Stmt& null(int i) { sqlite3_bind_null(s, i); return *this; }
    bool row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    bool done() { const int rc = s ? sqlite3_step(s) : SQLITE_ERROR; sqlite3_reset(s); sqlite3_clear_bindings(s); return rc == SQLITE_DONE; }
    std::int64_t i(int c) const { return sqlite3_column_int64(s, c); }
    std::string t(int c) const {
        const auto* p = sqlite3_column_text(s, c);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string{};
    }
};

struct Db {
    sqlite3* db = nullptr;
    ~Db() { if (db) sqlite3_close_v2(db); }
    bool exec(const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
    std::int64_t one(const char* sql) {
        Stmt q(db, sql);
        return q.row() ? q.i(0) : -1;
    }
    std::string err() const { return db ? sqlite3_errmsg(db) : "no database"; }
};

std::string path_of(const std::string& data_dir) { return data_dir + "/" + kFile; }

std::string stamp(std::int64_t now) {
    const std::time_t t = static_cast<std::time_t>(now);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

enum class Opened { Ok, Missing, Newer, Broken };

// Open read-write (creating when asked). Ours and current -> Ok. A file that
// isn't a database, or a database that isn't ours -> Broken (the caller sets
// it aside). A catalog from a newer Rouster -> Newer: never touched.
Opened open_rw(Db& d, const std::string& file, bool create, std::string& why) {
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
#ifdef SQLITE_OPEN_NOFOLLOW
    flags |= SQLITE_OPEN_NOFOLLOW;
#endif
    std::error_code ec;
    if (!create && !fs::exists(fs::symlink_status(file, ec))) return Opened::Missing;
    if (fs::is_symlink(fs::symlink_status(file, ec))) { why = "catalog.db is a link"; return Opened::Broken; }
    if (sqlite3_open_v2(file.c_str(), &d.db, flags, nullptr) != SQLITE_OK) { why = d.err(); return Opened::Broken; }
    sqlite3_busy_timeout(d.db, 5000);
    const std::int64_t tables = d.one("SELECT count(*) FROM sqlite_master");
    if (tables < 0) { why = d.err(); return Opened::Broken; }   // "file is not a database"
    const std::int64_t app = d.one("PRAGMA application_id");
    const std::int64_t ver = d.one("PRAGMA user_version");
    if (tables == 0) {
        if (!create) return Opened::Missing;
        const std::string init = "BEGIN; " + std::string(kSchemaSql) +
                                 "PRAGMA application_id = " + std::to_string(kAppId) + "; PRAGMA user_version = " +
                                 std::to_string(kSchema) + "; COMMIT;";
        if (!d.exec(init.c_str())) { why = d.err(); d.exec("ROLLBACK"); return Opened::Broken; }
        return Opened::Ok;
    }
    if (app != kAppId) { why = "not Rouster's catalog"; return Opened::Broken; }
    if (ver > kSchema) { why = "made by a newer Rouster (schema " + std::to_string(ver) + ")"; return Opened::Newer; }
    if (ver < 1)       { why = "no schema version"; return Opened::Broken; }
    return Opened::Ok;
}

Opened open_ro(Db& d, const std::string& data_dir) {
    const std::string file = path_of(data_dir);
    std::error_code ec;
    const auto st = fs::symlink_status(file, ec);
    if (!fs::is_regular_file(st)) return Opened::Missing;
    if (sqlite3_open_v2(file.c_str(), &d.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return Opened::Broken;
    sqlite3_busy_timeout(d.db, 5000);
    if (d.one("PRAGMA application_id") != kAppId) return Opened::Broken;
    const auto v = d.one("PRAGMA user_version");
    return v == kSchema ? Opened::Ok : (v > kSchema ? Opened::Newer : Opened::Broken);
}

std::string ext_of(const std::string& rel) {
    const auto slash = rel.rfind('/');
    const std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return {};
    std::string e = name.substr(dot + 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool under(const std::string& rel, const std::string& dir) {   // rel is dir or inside it ("" = everything)
    return dir.empty() || rel == dir || (rel.size() > dir.size() && rel.compare(0, dir.size(), dir) == 0 &&
                                         rel[dir.size()] == '/');
}

std::string unescape_mount(const std::string& s) {   // mountinfo writes ' ' as \040 etc.
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            std::isdigit(static_cast<unsigned char>(s[i + 1])) && std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            o += static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0'));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

}  // namespace

std::string atime_mode(const std::string& path) {
    std::error_code ec;
    const std::string p = fs::weakly_canonical(fs::path(path), ec).string();
    std::ifstream in("/proc/self/mountinfo");
    std::string line, best_point, best_opts;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string id, parent, devno, root, point, opts;
        if (!(ls >> id >> parent >> devno >> root >> point >> opts)) continue;
        point = unescape_mount(point);
        if (!under(p, point == "/" ? std::string{} : point)) continue;
        if (point.size() >= best_point.size()) { best_point = point; best_opts = opts; }
    }
    if (best_point.empty()) return {};
    const std::string o = "," + best_opts + ",";
    if (o.find(",noatime,") != std::string::npos) return "noatime";
    if (o.find(",relatime,") != std::string::npos) return "relatime";
    return "strictatime";
}

Delta record(const std::string& data_dir, const std::string& home, const std::string& root_rel,
             const std::vector<scan::Seen>& seen, const std::vector<std::string>& not_walked,
             std::int64_t now) {
    Delta out;
    if (data_dir.empty() || data_dir.front() != '/') { out.error = "no data folder"; return out; }
    std::error_code ec;
    fs::create_directories(data_dir, ec);   // Rouster's own folder
    const std::string file = path_of(data_dir);
    out.atime_mode = atime_mode(home);

    Db d;
    std::string why;
    Opened o = open_rw(d, file, true, why);
    if (o == Opened::Broken) {
        // Set aside, never deleted: the history and decisions in it may be wanted.
        if (d.db) { sqlite3_close_v2(d.db); d.db = nullptr; }
        const std::string aside = file + ".broken-" + stamp(now);
        fs::rename(file, aside, ec);
        if (ec) { out.error = "the catalog won't open (" + why + ") and couldn't be set aside: " + ec.message(); return out; }
        for (const char* side : {"-journal", "-wal", "-shm"}) fs::rename(file + side, aside + side, ec);
        out.set_aside = aside + " (" + why + ")";
        o = open_rw(d, file, true, why);
    }
    if (o == Opened::Newer) { out.error = "the catalog was " + why + " -- left alone"; return out; }
    if (o != Opened::Ok) { out.error = "the catalog won't open: " + why; return out; }
    out.began = d.one("SELECT count(*) FROM looks") == 0;

    if (!d.exec("BEGIN IMMEDIATE")) { out.error = d.err(); return out; }
    const auto fail = [&](const std::string& what) {
        out.error = what + ": " + d.err();
        d.exec("ROLLBACK");
        return out;
    };
    if (out.began) {
        Stmt m(d.db, "INSERT OR REPLACE INTO meta(key, value) VALUES ('began', ?)");
        if (!m.bind(1, std::to_string(now)).done()) return fail("meta");
    }
    {
        Stmt l(d.db, "INSERT INTO looks(at, root, atime_mode) VALUES (?, ?, ?)");
        if (!l.bind(1, now).bind(2, root_rel).bind(3, out.atime_mode).done()) return fail("look");
    }
    out.look = sqlite3_last_insert_rowid(d.db);

    struct Row { std::int64_t id, size, read, changed, gone; std::uint64_t dev, ino; std::string path; bool hit = false; };
    std::vector<Row> rows;
    {
        Stmt q(d.db, "SELECT id, path, size, last_read, last_changed, dev, ino, coalesce(gone, 0) FROM files");
        while (q.row())
            rows.push_back({q.i(0), q.i(2), q.i(3), q.i(4), q.i(7), static_cast<std::uint64_t>(q.i(5)),
                            static_cast<std::uint64_t>(q.i(6)), q.t(1)});
    }
    std::unordered_map<std::string, std::size_t> by_path;
    by_path.reserve(rows.size() * 2 + 1);
    for (std::size_t i = 0; i < rows.size(); ++i) by_path.emplace(rows[i].path, i);

    Stmt upd(d.db, "UPDATE files SET size = ?, last_read = ?, last_changed = ?, dev = ?, ino = ?, seen_look = ?, "
                   "gone = NULL, born = coalesce(born, nullif(?, 0)) WHERE id = ?");
    Stmt hist(d.db, "INSERT INTO history(file_id, look_id, last_read, last_changed, size) VALUES (?, ?, ?, ?, ?)");
    Stmt ins(d.db, "INSERT INTO files(path, package, ext, size, first_seen, born, last_read, last_changed, dev, ino, "
                   "seen_look) VALUES (?, ?, ?, ?, ?, nullif(?, 0), ?, ?, ?, ?, ?)");
    Stmt mv(d.db, "UPDATE files SET path = ?, ext = ?, size = ?, last_read = ?, seen_look = ?, gone = NULL WHERE id = ?");
    Stmt mvlog(d.db, "INSERT INTO moves(file_id, look_id, from_path, to_path) VALUES (?, ?, ?, ?)");
    Stmt mvdec(d.db, "UPDATE decisions SET path = ? WHERE file_id = ?");
    Stmt gone(d.db, "UPDATE files SET gone = ? WHERE id = ?");
    if (!upd || !hist || !ins || !mv || !mvlog || !mvdec || !gone) return fail("prepare");

    const auto i64 = [](std::uint64_t v) { return static_cast<std::int64_t>(v); };
    std::vector<const scan::Seen*> fresh;
    for (const auto& s : seen) {
        ++out.files;
        if (s.package) ++out.packages;
        const auto f = by_path.find(s.rel);
        if (f == by_path.end()) { fresh.push_back(&s); continue; }
        Row& r = rows[f->second];
        r.hit = true;
        const bool back = r.gone != 0;
        const bool changed = r.changed != s.changed || r.size != i64(s.bytes);
        const bool read = !changed && s.read > r.read;
        if (back) ++out.returned;
        else if (changed) ++out.changed;
        else if (read) ++out.read;
        if (!upd.bind(1, i64(s.bytes)).bind(2, s.read).bind(3, s.changed).bind(4, i64(s.dev)).bind(5, i64(s.ino))
                 .bind(6, out.look).bind(7, s.born).bind(8, r.id).done())
            return fail("update");
        if ((back || changed || read) &&
            !hist.bind(1, r.id).bind(2, out.look).bind(3, s.read).bind(4, s.changed).bind(5, i64(s.bytes)).done())
            return fail("history");
    }

    // Missed this Look: in scope, not seen, not already gone, and somewhere a
    // Look actually walked. A missed row may turn out to have moved.
    const auto walked = [&](const std::string& rel) {
        if (!under(rel, root_rel)) return false;
        for (const auto& nw : not_walked) if (under(rel, nw)) return false;
        return true;
    };
    std::multimap<std::pair<std::uint64_t, std::uint64_t>, std::size_t> missed;
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].hit && rows[i].gone == 0 && walked(rows[i].path))
            missed.emplace(std::make_pair(rows[i].dev, rows[i].ino), i);

    for (const scan::Seen* s : fresh) {
        std::size_t was = rows.size();
        auto [a, b] = missed.equal_range({s->dev, s->ino});
        for (auto it = a; it != b; ++it) {
            const Row& r = rows[it->second];
            if (r.size == i64(s->bytes) && r.changed == s->changed) { was = it->second; missed.erase(it); break; }
        }
        if (was != rows.size()) {   // the same file, somewhere else
            Row& r = rows[was];
            r.hit = true;
            ++out.moved;
            if (!mv.bind(1, s->rel).bind(2, ext_of(s->rel)).bind(3, i64(s->bytes)).bind(4, s->read).bind(5, out.look)
                     .bind(6, r.id).done() ||
                !mvlog.bind(1, r.id).bind(2, out.look).bind(3, r.path).bind(4, s->rel).done() ||
                !mvdec.bind(1, s->rel).bind(2, r.id).done())
                return fail("move");
            continue;
        }
        ++out.fresh;
        if (!ins.bind(1, s->rel).bind(2, s->package ? 1 : 0).bind(3, ext_of(s->rel)).bind(4, i64(s->bytes))
                 .bind(5, now).bind(6, s->born).bind(7, s->read).bind(8, s->changed).bind(9, i64(s->dev))
                 .bind(10, i64(s->ino)).bind(11, out.look).done())
            return fail("insert");
        if (!hist.bind(1, sqlite3_last_insert_rowid(d.db)).bind(2, out.look).bind(3, s->read).bind(4, s->changed)
                 .bind(5, i64(s->bytes)).done())
            return fail("history");
    }
    for (const auto& [key, i] : missed) {
        ++out.gone;
        if (!gone.bind(1, now).bind(2, rows[i].id).done()) return fail("gone");
    }
    {
        Stmt l(d.db, "UPDATE looks SET files = ?, fresh = ?, changed = ?, read = ?, moved = ?, gone = ?, returned = ? "
                     "WHERE id = ?");
        if (!l.bind(1, i64(out.files)).bind(2, i64(out.fresh)).bind(3, i64(out.changed)).bind(4, i64(out.read))
                 .bind(5, i64(out.moved)).bind(6, i64(out.gone)).bind(7, i64(out.returned)).bind(8, out.look).done())
            return fail("look");
    }
    if (!d.exec("COMMIT")) return fail("commit");
    out.rows = static_cast<std::uint64_t>(std::max<std::int64_t>(0, d.one("SELECT count(*) FROM files")));
    out.ok = true;
    return out;
}

Summary summary(const std::string& data_dir) {
    Summary s;
    Db d;
    if (open_ro(d, data_dir) != Opened::Ok) return s;
    s.files     = static_cast<std::uint64_t>(d.one("SELECT count(*) FROM files WHERE gone IS NULL"));
    s.packages  = static_cast<std::uint64_t>(d.one("SELECT count(*) FROM files WHERE gone IS NULL AND package = 1"));
    s.gone      = static_cast<std::uint64_t>(d.one("SELECT count(*) FROM files WHERE gone IS NOT NULL"));
    s.looks     = static_cast<std::uint64_t>(d.one("SELECT count(*) FROM looks"));
    s.decisions = static_cast<std::uint64_t>(d.one("SELECT count(*) FROM decisions"));
    s.began     = d.one("SELECT CAST(value AS INTEGER) FROM meta WHERE key = 'began'");
    s.last_look = d.one("SELECT coalesce(max(at), 0) FROM looks");
    s.ok = true;
    return s;
}

std::optional<File> find(const std::string& data_dir, const std::string& rel) {
    Db d;
    if (open_ro(d, data_dir) != Opened::Ok) return std::nullopt;
    Stmt q(d.db, "SELECT id, path, package, coalesce(ext, ''), size, first_seen, coalesce(born, 0), last_read, "
                 "last_changed, coalesce(gone, 0) FROM files WHERE path = ?");
    q.bind(1, rel);
    if (!q.row()) return std::nullopt;
    return File{q.i(0), q.t(1), q.i(2) != 0, q.t(3), static_cast<std::uint64_t>(q.i(4)), q.i(5), q.i(6), q.i(7),
                q.i(8), q.i(9)};
}

std::vector<Moment> history(const std::string& data_dir, const std::string& rel) {
    std::vector<Moment> v;
    Db d;
    if (open_ro(d, data_dir) != Opened::Ok) return v;
    Stmt q(d.db, "SELECT l.at, h.last_read, h.last_changed, h.size FROM history h JOIN looks l ON l.id = h.look_id "
                 "WHERE h.file_id = (SELECT id FROM files WHERE path = ?) ORDER BY h.look_id");
    q.bind(1, rel);
    while (q.row()) v.push_back({q.i(0), q.i(1), q.i(2), static_cast<std::uint64_t>(q.i(3))});
    return v;
}

std::vector<Move> moves(const std::string& data_dir, const std::string& rel) {
    std::vector<Move> v;
    Db d;
    if (open_ro(d, data_dir) != Opened::Ok) return v;
    Stmt q(d.db, "SELECT l.at, m.from_path, m.to_path FROM moves m JOIN looks l ON l.id = m.look_id "
                 "WHERE m.file_id = (SELECT id FROM files WHERE path = ?) ORDER BY m.look_id");
    q.bind(1, rel);
    while (q.row()) v.push_back({q.i(0), q.t(1), q.t(2)});
    return v;
}

bool valid_verdict(const std::string& v) {
    if (v.empty() || v.size() > 32 || v.front() == '-' || v.back() == '-') return false;
    return std::all_of(v.begin(), v.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
}

bool decide(const std::string& data_dir, const std::string& rel, const std::string& verdict,
            const std::string& detail, std::int64_t now, std::string* why) {
    const auto no = [&](std::string w) { if (why) *why = std::move(w); return false; };
    if (!valid_verdict(verdict)) return no("not a verdict: '" + verdict + "'");
    if (rel.empty() || rel.front() == '/' || rel.find("..") != std::string::npos) return no("not a path under home: " + rel);
    if (detail.size() > 1000) return no("the note is too long");
    Db d;
    std::string w;
    if (open_rw(d, path_of(data_dir), false, w) != Opened::Ok) return no("no catalog yet -- Look first" + (w.empty() ? "" : " (" + w + ")"));
    Stmt q(d.db, "INSERT INTO decisions(path, file_id, verdict, detail, made_at) "
                 "VALUES (?1, (SELECT id FROM files WHERE path = ?1), ?2, ?3, ?4)");
    if (!q.bind(1, rel).bind(2, verdict).bind(3, detail).bind(4, now).done()) return no(d.err());
    return true;
}

std::vector<Decision> decisions(const std::string& data_dir) {
    std::vector<Decision> v;
    Db d;
    if (open_ro(d, data_dir) != Opened::Ok) return v;
    Stmt q(d.db, "SELECT id, made_at, path, verdict, coalesce(detail, '') FROM decisions ORDER BY id DESC");
    while (q.row()) v.push_back({q.i(0), q.i(1), q.t(2), q.t(3), q.t(4)});
    return v;
}

}  // namespace rouster::core::catalog
