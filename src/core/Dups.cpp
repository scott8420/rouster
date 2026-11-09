#include "core/Dups.hpp"
#include "core/Package.hpp"
#include "core/Scan.hpp"   // classify_dir: Protected / Regenerable are the scan's policy

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <regex>
#include <tuple>
#include <unordered_map>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

// Dups.cpp -- READ-ONLY. Opens files O_RDONLY to hash them and asks FIEMAP
// where their blocks are. Nothing here writes; the door's Dedupe verb shares
// the blocks, its Trash verb moves a copy.

namespace rouster::core::dups {

namespace fs = std::filesystem;

const char* stage_name(int s) {
    switch (s) {
        case 0: return "Looking at files";
        case 1: return "Comparing beginnings and ends";
        case 2: return "Comparing whole files";
        case 3: return "Matching folders";
    }
    return "";
}

bool copy_like(const std::string& name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("copy of ", 0) == 0) return true;
    const auto dot = n.rfind('.');
    const std::string stem = (dot != std::string::npos && dot > 0) ? n.substr(0, dot) : n;
    // (written without "copy" next to an open paren: the source gate reads that as a call)
    static const std::regex tail(R"(( \(\d+\)|[ _-]copy\s?\d*|\(copy\)| - copy\s?\(?\d*\)?)$)");
    return std::regex_search(stem, tail);
}

bool Group::shared(std::size_t i) const {
    return i < members.size() && keep < members.size() && members[i].extents != 0 &&
           members[i].extents == members[keep].extents;
}

std::uint64_t Group::gain(Mode m) const {
    std::uint64_t g = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i == keep || !members[i].checked) continue;
        if (m == Mode::Share && shared(i)) continue;
        g += bytes;
    }
    return g;
}

namespace {

bool starts_dir(const std::string& rel, const char* top) {
    const std::size_t n = std::strlen(top);
    return rel.compare(0, n, top) == 0 && (rel.size() == n || rel[n] == '/');
}

// Lower is better: the keep rules of §4.4, in order.
auto rank(const Member& m) {
    const std::string name = fs::path(m.rel).filename().string();
    const int place = starts_dir(m.rel, "Downloads") ? 2 : starts_dir(m.rel, "Desktop") ? 1 : 0;
    return std::make_tuple(place, copy_like(name) ? 1 : 0, m.mtime_ns, m.rel.size(), m.rel);
}

}  // namespace

void choose_keeper(Group& g) {
    if (g.members.empty()) return;
    std::vector<std::size_t> idx(g.members.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](auto a, auto b) { return rank(g.members[a]) < rank(g.members[b]); });
    g.keep = idx[0];
    const auto a = rank(g.members[idx[0]]), b = rank(g.members[idx[1]]);
    if (std::get<0>(a) != std::get<0>(b))
        g.keep_why = std::get<0>(b) == 2 ? "not in Downloads" : "not on the Desktop";
    else if (std::get<1>(a) != std::get<1>(b)) g.keep_why = "the original name";
    else if (std::get<2>(a) != std::get<2>(b)) g.keep_why = "the oldest";
    else if (std::get<3>(a) != std::get<3>(b)) g.keep_why = "the shortest path";
    else g.keep_why = "first by name";
}

bool can_share(const std::string& path) {
    struct statfs s{};
    if (::statfs(path.c_str(), &s) != 0) return false;
    return static_cast<unsigned long>(s.f_type) == 0x9123683EUL;   // BTRFS_SUPER_MAGIC
}

namespace {

struct H {   // a content key; ordered, so it can key a std::map
    std::uint64_t high64 = 0, low64 = 0;
    H() = default;
    H(std::uint64_t h, std::uint64_t l) : high64(h), low64(l) {}
    H(const XXH128_hash_t& x) : high64(x.high64), low64(x.low64) {}
    auto operator<=>(const H&) const = default;
};

std::string hex(const H& h) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(h.high64),
                  static_cast<unsigned long long>(h.low64));
    return buf;
}

int open_ro(const std::string& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOATIME | O_NOFOLLOW);
    if (fd < 0 && errno == EPERM) fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    return fd;
}

// Hash [0, len) and, when whole is false, also [size-len, size). false = unreadable.
bool hash_file(const std::string& p, std::uint64_t size, bool whole, H& out, Progress& prog) {
    const int fd = open_ro(p);
    if (fd < 0) return false;
    XXH3_state_t* st = XXH3_createState();
    XXH3_128bits_reset(st);
    static thread_local std::vector<char> buf(1 << 20);
    bool ok = true;
    const auto take = [&](std::uint64_t off, std::uint64_t len) {
        while (len > 0 && ok) {
            if (prog.cancel) { ok = false; return; }
            const ssize_t n = ::pread(fd, buf.data(), std::min<std::uint64_t>(len, buf.size()), static_cast<off_t>(off));
            if (n <= 0) { ok = false; return; }
            XXH3_128bits_update(st, buf.data(), static_cast<std::size_t>(n));
            off += static_cast<std::uint64_t>(n);
            len -= static_cast<std::uint64_t>(n);
            prog.hashed += static_cast<std::uint64_t>(n);
        }
    };
    if (whole) take(0, size);
    else { take(0, kEdge); take(size - kEdge, kEdge); }
    out = XXH3_128bits_digest(st);
    XXH3_freeState(st);
    ::close(fd);
    return ok;
}

// Where the file's blocks are, as one number. 0 = unknown (inline, not yet
// placed, unreadable). Two files with the same non-zero print share blocks.
std::uint64_t extent_print(const std::string& p) {
    const int fd = open_ro(p);
    if (fd < 0) return 0;
    constexpr int kMax = 128;
    std::vector<char> buf(sizeof(fiemap) + kMax * sizeof(fiemap_extent));
    auto* fm = reinterpret_cast<fiemap*>(buf.data());
    fm->fm_start = 0;
    fm->fm_length = ~0ULL;
    fm->fm_flags = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = kMax;
    std::uint64_t print = 0;
    if (::ioctl(fd, FS_IOC_FIEMAP, fm) == 0 && fm->fm_mapped_extents > 0) {
        XXH3_state_t* st = XXH3_createState();
        XXH3_64bits_reset(st);
        bool known = true;
        for (std::uint32_t i = 0; i < fm->fm_mapped_extents; ++i) {
            const auto& e = fm->fm_extents[i];
            if (e.fe_flags & (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_DATA_INLINE |
                              FIEMAP_EXTENT_NOT_ALIGNED)) { known = false; break; }
            const std::uint64_t v[3] = {e.fe_logical, e.fe_physical, e.fe_length};
            XXH3_64bits_update(st, v, sizeof v);
        }
        if (known && fm->fm_mapped_extents < static_cast<std::uint32_t>(kMax)) print = XXH3_64bits_digest(st) | 1;
        XXH3_freeState(st);
    }
    ::close(fd);
    return print;
}

struct FRec {
    std::string   rel, name;
    std::uint64_t size = 0, dev = 0, ino = 0;
    std::int64_t  mtime_ns = 0;
    int           dir = -1;
    int           link_of = -1;     // a hard link to an earlier file
    bool          keyed = false;    // its content equals some other file's
    H             key{};
};
struct DRec {
    std::string   rel, name;
    int           parent = -1;
    scan::Kind    kind = scan::Kind::Other;
    std::uint64_t dev = 0, ino = 0;
    std::int64_t  mtime_ns = 0;
    bool          clean = true;     // nothing in it was skipped
    std::vector<int> files, dirs;
    bool          signable = false;
    H             sig{};
    std::uint64_t bytes = 0, nfiles = 0;
};

std::int64_t mt(const struct stat& st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

bool looks_like_build(const std::string& abs) {
    struct stat st{};
    return ::lstat((abs + "/CMakeCache.txt").c_str(), &st) == 0 || ::lstat((abs + "/build.ninja").c_str(), &st) == 0 ||
           ::lstat((abs + "/meson-info").c_str(), &st) == 0;
}

void inner_files(const std::vector<DRec>& D, const std::vector<FRec>& F, int d, const std::string& prefix,
                 std::vector<std::pair<std::string, std::uint64_t>>& out) {
    for (int f : D[d].files) out.emplace_back(prefix + F[f].name, F[f].size);
    for (int c : D[d].dirs) inner_files(D, F, c, prefix + D[c].name + "/", out);
}

}  // namespace

bool same_blocks(const std::string& a, const std::string& b) {
    const std::uint64_t pa = extent_print(a);
    return pa != 0 && pa == extent_print(b);
}

Found find(const std::string& home_in, const std::string& root_in, Progress& prog) {
    Found out;
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    const std::string root = fs::weakly_canonical(root_in, ec).string();
    const auto done = [&] { prog.done = true; return out; };
    struct stat rs{};
    if (home.empty() || home == "/") { out.refused = "home is /"; return done(); }
    if (!(root == home || (root.rfind(home + "/", 0) == 0))) { out.refused = "outside the home folder: " + root; return done(); }
    if (::lstat(root.c_str(), &rs) != 0 || !S_ISDIR(rs.st_mode)) { out.refused = "not a folder: " + root; return done(); }
    out.reflink = can_share(root);

    std::vector<FRec> F;
    std::vector<DRec> D;
    {
        DRec r;
        r.rel = root == home ? "" : root.substr(home.size() + 1);
        r.name = fs::path(root).filename().string();
        // the root's kind, from home down
        scan::Kind k = scan::Kind::Other;
        if (!r.rel.empty()) {
            std::size_t pos = 0;
            while (true) {
                const auto s = r.rel.find('/', pos);
                k = scan::classify_dir(r.rel.substr(0, s), k, false).kind;
                if (s == std::string::npos) break;
                pos = s + 1;
            }
        }
        if (k == scan::Kind::Protected) { out.refused = "protected (never touched): ~/" + r.rel; return done(); }
        if (const auto p = pkg::package_of(r.rel, true); !p.empty()) {
            out.refused = "a package is kept whole, never looked inside: ~/" + p;
            return done();
        }
        r.kind = k;
        r.dev = rs.st_dev; r.ino = rs.st_ino; r.mtime_ns = mt(rs);
        D.push_back(r);
    }
    std::map<std::pair<std::uint64_t, std::uint64_t>, int> inodes;

    // 1. Walk.
    std::vector<int> stack{0};
    while (!stack.empty() && !prog.cancel) {
        const int di = stack.back();
        stack.pop_back();
        const std::string dabs = D[di].rel.empty() ? home : home + "/" + D[di].rel;
        DIR* dir = ::opendir(dabs.c_str());
        if (!dir) { ++out.errors; D[di].clean = false; continue; }
        std::vector<std::string> names;
        while (dirent* e = ::readdir(dir)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            names.push_back(n);
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());
        for (const auto& n : names) {
            if (n[0] == '.') { D[di].clean = false; continue; }          // hidden: not read, so not vouched for
            const std::string rel = D[di].rel.empty() ? n : D[di].rel + "/" + n;
            const std::string abs = home + "/" + rel;
            struct stat st{};
            if (::lstat(abs.c_str(), &st) != 0) { ++out.errors; D[di].clean = false; continue; }
            if (S_ISDIR(st.st_mode)) {
                if (st.st_dev != rs.st_dev || n == "node_modules") { D[di].clean = false; continue; }
                if (pkg::is_package_name(n)) {   // s015a: kept whole -- never entered, never paired
                    out.packages.push_back(rel);
                    D[di].clean = false;
                    continue;
                }
                const scan::Kind k = scan::classify_dir(rel, D[di].kind, looks_like_build(abs)).kind;
                if (k == scan::Kind::Protected || k == scan::Kind::Regenerable || k == scan::Kind::Trash) {
                    D[di].clean = false;
                    continue;
                }
                DRec d;
                d.rel = rel; d.name = n; d.parent = di; d.kind = k;
                d.dev = st.st_dev; d.ino = st.st_ino; d.mtime_ns = mt(st);
                D[di].dirs.push_back(static_cast<int>(D.size()));
                stack.push_back(static_cast<int>(D.size()));
                D.push_back(std::move(d));
            } else if (S_ISREG(st.st_mode)) {
                FRec f;
                f.rel = rel; f.name = n; f.dir = di;
                f.size = static_cast<std::uint64_t>(st.st_size);
                f.dev = st.st_dev; f.ino = st.st_ino; f.mtime_ns = mt(st);
                const auto [it, fresh] = inodes.try_emplace({f.dev, f.ino}, static_cast<int>(F.size()));
                if (!fresh) f.link_of = it->second;
                D[di].files.push_back(static_cast<int>(F.size()));
                F.push_back(std::move(f));
                ++prog.files;
            } else {
                D[di].clean = false;                                     // symlink, fifo, socket, device
            }
        }
    }
    out.files = F.size();
    if (prog.cancel) { out.cancelled = true; return done(); }

    // 2-4. Size -> head/tail -> whole.
    prog.stage = 1;
    std::map<std::uint64_t, std::vector<int>> by_size;
    for (int i = 0; i < static_cast<int>(F.size()); ++i)
        if (F[i].link_of < 0) by_size[F[i].size].push_back(i);
    std::map<std::pair<std::uint64_t, H>, std::vector<int>> by_full;
    const H empty_key{0x9e3779b97f4a7c15ULL, 0};
    for (auto& [size, v] : by_size) {
        if (prog.cancel) break;
        if (size == 0) {   // every empty file is the same content; no read needed
            for (int i : v) { F[i].key = empty_key; F[i].keyed = true; }
            continue;
        }
        if (v.size() < 2) continue;
        if (size <= 2 * kEdge) {
            for (int i : v) {
                H h{};
                if (hash_file(home + "/" + F[i].rel, size, true, h, prog)) by_full[{size, h}].push_back(i);
                else ++out.errors;
            }
            continue;
        }
        std::map<H, std::vector<int>> by_edge;
        for (int i : v) {
            H h{};
            if (hash_file(home + "/" + F[i].rel, size, false, h, prog)) by_edge[h].push_back(i);
            else ++out.errors;
        }
        prog.stage = 2;
        for (auto& [eh, w] : by_edge) {
            if (w.size() < 2) continue;
            for (int i : w) {
                H h{};
                if (hash_file(home + "/" + F[i].rel, size, true, h, prog)) by_full[{size, h}].push_back(i);
                else ++out.errors;
            }
        }
        prog.stage = 1;
    }
    if (prog.cancel) { out.cancelled = true; out.hashed = prog.hashed; return done(); }
    for (auto& [k, v] : by_full)
        if (v.size() >= 2)
            for (int i : v) { F[i].key = k.second; F[i].keyed = true; }
    for (auto& f : F)
        if (f.link_of >= 0) { f.key = F[f.link_of].key; f.keyed = F[f.link_of].keyed; }

    // Folders, bottom-up (a child's index is always after its parent's).
    prog.stage = 3;
    for (int d = static_cast<int>(D.size()) - 1; d >= 0; --d) {
        DRec& r = D[d];
        r.signable = r.clean;
        std::vector<std::string> ent;
        for (int f : r.files) {
            if (!F[f].keyed) { r.signable = false; break; }
            ent.push_back("F" + F[f].name + '\0' + hex(F[f].key));
            r.bytes += F[f].size;
            ++r.nfiles;
        }
        for (int c : r.dirs) {
            if (!r.signable) break;
            if (!D[c].signable) { r.signable = false; break; }
            ent.push_back("D" + D[c].name + '\0' + hex(D[c].sig));
            r.bytes += D[c].bytes;
            r.nfiles += D[c].nfiles;
        }
        if (!r.signable) continue;
        std::sort(ent.begin(), ent.end());
        XXH3_state_t* st = XXH3_createState();
        XXH3_128bits_reset(st);
        for (const auto& e : ent) XXH3_128bits_update(st, e.data(), e.size() + 1);
        r.sig = XXH3_128bits_digest(st);
        XXH3_freeState(st);
    }
    std::map<H, std::vector<int>> by_sig;
    for (int d = 1; d < static_cast<int>(D.size()); ++d)
        if (D[d].signable && D[d].nfiles > 0 && D[d].bytes >= kMinBytes) by_sig[D[d].sig].push_back(d);
    std::vector<char> dupdir(D.size(), 0);
    for (auto& [s, v] : by_sig)
        if (v.size() >= 2) for (int d : v) dupdir[d] = 1;

    std::vector<std::string> copies_of_folders;   // filled after keepers are chosen
    for (auto& [s, v] : by_sig) {
        if (v.size() < 2) continue;
        // only the outermost copies: some member's parent is not itself a copy
        const bool outer = std::any_of(v.begin(), v.end(), [&](int d) { return D[d].parent < 0 || !dupdir[D[d].parent]; });
        if (!outer) continue;
        Group g;
        g.folder = true;
        g.bytes = D[v[0]].bytes;
        g.files = D[v[0]].nfiles;
        for (int d : v) {
            Member m;
            m.rel = D[d].rel; m.mtime_ns = D[d].mtime_ns; m.dev = D[d].dev; m.ino = D[d].ino; m.size = D[d].bytes;
            g.members.push_back(m);
        }
        std::vector<std::pair<std::string, std::uint64_t>> in;
        inner_files(D, F, v[0], "", in);
        std::sort(in.begin(), in.end());
        for (auto& [p, sz] : in) { g.inner.push_back(p); g.inner_size.push_back(sz); }
        choose_keeper(g);
        for (std::size_t i = 0; i < g.members.size(); ++i)
            if (i != g.keep) copies_of_folders.push_back(g.members[i].rel);
        out.groups.push_back(std::move(g));
    }
    const auto in_folder_copy = [&](const std::string& rel) {
        for (const auto& c : copies_of_folders)
            if (rel.size() > c.size() && rel.compare(0, c.size(), c) == 0 && rel[c.size()] == '/') return true;
        return false;
    };
    for (auto& [k, v] : by_full) {
        if (v.size() < 2 || k.first < kMinBytes) continue;
        Group g;
        g.bytes = k.first;
        for (int i : v) {
            if (in_folder_copy(F[i].rel)) continue;
            Member m;
            m.rel = F[i].rel; m.mtime_ns = F[i].mtime_ns; m.dev = F[i].dev; m.ino = F[i].ino; m.size = F[i].size;
            g.members.push_back(m);
        }
        if (g.members.size() < 2) continue;
        choose_keeper(g);
        out.groups.push_back(std::move(g));
    }

    // Already sharing? Only where sharing is possible.
    if (out.reflink) {
        for (auto& g : out.groups) {
            for (auto& m : g.members) {
                if (prog.cancel) break;
                if (!g.folder) { m.extents = extent_print(home + "/" + m.rel); continue; }
                XXH64_hash_t acc = 0;
                bool known = true;
                for (std::size_t j = 0; j < g.inner.size() && known; ++j) {
                    if (g.inner_size[j] < kMinBytes) continue;
                    const std::uint64_t p = extent_print(home + "/" + m.rel + "/" + g.inner[j]);
                    if (p == 0) known = false;
                    acc = XXH64(&p, sizeof p, acc);
                }
                m.extents = known ? (acc | 1) : 0;
            }
        }
    }
    preset(out, out.reflink ? Mode::Share : Mode::Trash);
    const auto key = [](const Group& g) { return std::make_tuple(!g.folder, -static_cast<std::int64_t>(g.bytes * (g.members.size() - 1)), g.members[g.keep].rel); };
    std::sort(out.groups.begin(), out.groups.end(), [&](const Group& a, const Group& b) { return key(a) < key(b); });
    out.hashed = prog.hashed;
    out.cancelled = prog.cancel;
    return done();
}

void preset(Found& f, Mode mode) {
    for (auto& g : f.groups)
        for (std::size_t i = 0; i < g.members.size(); ++i)
            g.members[i].checked = mode == Mode::Share && i != g.keep && !g.shared(i);
}

door::Plan plan(const std::string& home_in, const Found& f, Mode mode, std::vector<std::string>& dropped) {
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    door::Plan p;
    const auto same = [&](const Member& m, bool folder) {
        struct stat st{};
        if (::lstat((home + "/" + m.rel).c_str(), &st) != 0) return false;
        if (folder) return S_ISDIR(st.st_mode) && st.st_dev == m.dev && st.st_ino == m.ino;
        return S_ISREG(st.st_mode) && st.st_dev == m.dev && st.st_ino == m.ino &&
               static_cast<std::uint64_t>(st.st_size) == m.size && mt(st) == m.mtime_ns;
    };
    const auto inner_ok = [&](const std::string& rel, const Group& g) {
        for (std::size_t j = 0; j < g.inner.size(); ++j) {
            struct stat st{};
            if (::lstat((home + "/" + rel + "/" + g.inner[j]).c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
                static_cast<std::uint64_t>(st.st_size) != g.inner_size[j]) return false;
        }
        return true;
    };
    struct Planned { std::size_t group; door::Action a; };
    std::vector<Planned> acts;
    for (std::size_t gi = 0; gi < f.groups.size(); ++gi) {
        const Group& g = f.groups[gi];
        const Member& k = g.members[g.keep];
        bool any = false;
        for (std::size_t i = 0; i < g.members.size(); ++i)
            if (i != g.keep && g.members[i].checked && !(mode == Mode::Share && g.shared(i))) any = true;
        if (!any) continue;
        if (!same(k, g.folder) || (g.folder && mode == Mode::Share && !inner_ok(k.rel, g))) {
            dropped.push_back("~/" + k.rel + " changed since Look -- its group was left out");
            continue;
        }
        for (std::size_t i = 0; i < g.members.size(); ++i) {
            const Member& m = g.members[i];
            if (i == g.keep || !m.checked || (mode == Mode::Share && g.shared(i))) continue;
            if (!same(m, g.folder) || (g.folder && mode == Mode::Share && !inner_ok(m.rel, g))) {
                dropped.push_back("~/" + m.rel + " changed since Look -- left out");
                continue;
            }
            if (mode == Mode::Trash) {
                door::Action a;
                a.verb = door::Verb::Trash;
                a.from = home + "/" + m.rel;
                a.bytes = g.bytes;
                a.reason = "a copy of ~/" + k.rel;
                acts.push_back({gi, a});
            } else if (!g.folder) {
                door::Action a;
                a.verb = door::Verb::Dedupe;
                a.from = home + "/" + k.rel;
                a.to = home + "/" + m.rel;
                a.bytes = g.bytes;
                a.reason = "share blocks with ~/" + k.rel;
                acts.push_back({gi, a});
            } else {
                for (std::size_t j = 0; j < g.inner.size(); ++j) {
                    if (g.inner_size[j] < kMinBytes) continue;
                    struct stat s1{}, s2{};   // a hard link already IS one copy
                    if (::lstat((home + "/" + k.rel + "/" + g.inner[j]).c_str(), &s1) == 0 &&
                        ::lstat((home + "/" + m.rel + "/" + g.inner[j]).c_str(), &s2) == 0 &&
                        s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino) continue;
                    door::Action a;
                    a.verb = door::Verb::Dedupe;
                    a.from = home + "/" + k.rel + "/" + g.inner[j];
                    a.to = home + "/" + m.rel + "/" + g.inner[j];
                    a.bytes = g.inner_size[j];
                    a.reason = "share blocks with ~/" + k.rel + "/" + g.inner[j];
                    acts.push_back({gi, a});
                }
            }
        }
    }
    if (mode == Mode::Trash) {
        const auto under = [](const std::string& p, const std::string& d) {
            return p == d || (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/');
        };
        // A keeper must never go to the Trash with its copies: if it sits in (or
        // is) something trashed, the whole group stays.
        std::vector<char> drop_group(f.groups.size(), 0);
        for (const auto& pa : acts) {
            const Group& g = f.groups[pa.group];
            const std::string kabs = home + "/" + g.members[g.keep].rel;
            for (const auto& other : acts)
                if (under(kabs, other.a.from)) { drop_group[pa.group] = 1; break; }
        }
        std::vector<Planned> kept;
        for (auto& pa : acts) {
            if (drop_group[pa.group]) continue;
            kept.push_back(pa);
        }
        for (std::size_t gi = 0; gi < f.groups.size(); ++gi)
            if (drop_group[gi])
                dropped.push_back("~/" + f.groups[gi].members[f.groups[gi].keep].rel +
                                  " would go to the Trash with its copies -- its group was left out");
        // Inside something already going: it goes with it.
        acts.clear();
        for (auto& pa : kept) {
            bool nested = false;
            for (const auto& other : kept)
                if (&other != &pa && pa.a.from != other.a.from && under(pa.a.from, other.a.from)) nested = true;
            if (!nested) acts.push_back(pa);
        }
    }
    std::size_t copies = 0;
    for (std::size_t gi = 0; gi < f.groups.size(); ++gi) {
        bool in = false;
        for (const auto& pa : acts) if (pa.group == gi) in = true;
        if (!in) continue;
        for (std::size_t i = 0; i < f.groups[gi].members.size(); ++i)
            if (i != f.groups[gi].keep && f.groups[gi].members[i].checked) ++copies;
    }
    for (auto& pa : acts) p.actions.push_back(std::move(pa.a));
    const std::string n = std::to_string(copies) + (copies == 1 ? " copy" : " copies");
    p.label = mode == Mode::Share ? "Duplicates: share blocks of " + n : "Duplicates: " + n + " to the Trash";
    return p;
}

}  // namespace rouster::core::dups
