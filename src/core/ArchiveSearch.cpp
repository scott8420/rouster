// ArchiveSearch.cpp (s018) -- READ-ONLY: opendir + lstat over ~/Archive, a
// read-only catalog query, Rouster's records as stored() read them.
#include "core/Archive.hpp"
#include "core/Catalog.hpp"
#include "core/Package.hpp"
#include "core/Scan.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>

#include <dirent.h>
#include <sys/stat.h>

namespace rouster::core::archive {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string ext_of(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return "";
    return lower(name.substr(dot + 1));
}
bool in(const std::string& e, std::initializer_list<const char*> l) {
    for (const char* x : l) if (e == x) return true;
    return false;
}

}  // namespace

const std::vector<std::string>& search_kinds() {
    static const std::vector<std::string> v = {"Documents", "Pictures", "Music", "Video", "Text & code", "Archives", "Other"};
    return v;
}

std::string search_kind(const std::string& ext_in, const std::string& ct) {
    const std::string e = lower(ext_in);
    // A few the content type gets "wrong" for us: an ODT is a document even
    // though it's a zip; a .scriv bundle has no content type at all.
    if (in(e, {"pdf", "odt", "ott", "fodt", "doc", "docx", "rtf", "pages", "scriv", "epub", "mobi", "azw3", "xls",
               "xlsx", "ods", "numbers", "ppt", "pptx", "odp", "key", "fdx", "fountain", "folio", "optik", "jots"}))
        return "Documents";
    if (in(e, {"zip", "tar", "gz", "tgz", "bz2", "xz", "zst", "7z", "rar", "iso", "dmg", "img", "deb", "rpm", "jar"}))
        return "Archives";
    const auto slash = ct.find('/');
    const std::string major = slash == std::string::npos ? "" : ct.substr(0, slash);
    if (major == "image") return "Pictures";
    if (major == "audio") return "Music";
    if (major == "video") return "Video";
    if (major == "text") return "Text & code";
    if (in(e, {"png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff", "svg", "heic", "heif", "avif", "raw", "cr2",
               "nef", "dng", "psd", "xcf", "kra", "ora", "ico", "curvz"}))
        return "Pictures";
    if (in(e, {"mp3", "flac", "ogg", "oga", "opus", "wav", "m4a", "aac", "aiff", "wma", "mid", "midi"})) return "Music";
    if (in(e, {"mp4", "mkv", "mov", "avi", "webm", "m4v", "wmv", "mpg", "mpeg", "3gp"})) return "Video";
    if (in(e, {"txt", "md", "markdown", "rst", "org", "csv", "tsv", "json", "xml", "html", "htm", "css", "js", "ts",
               "py", "sh", "c", "h", "cc", "cpp", "hpp", "cxx", "rs", "go", "java", "ini", "conf", "cfg", "toml",
               "yaml", "yml", "log", "tex", "srt", "sql"}))
        return "Text & code";
    return "Other";
}

bool Query::empty() const {
    return kind.empty() && year == 0 && std::all_of(text.begin(), text.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

Index index(const std::string& home_in, const std::string& data_dir, const std::vector<Stored>& stored) {
    Index ix;
    std::error_code ec;
    const std::string home = fs::weakly_canonical(home_in, ec).string();
    const std::string root = home + "/" + kRoot;
    struct stat rst{};
    if (::lstat(root.c_str(), &rst) != 0) { ix.ok = true; return ix; }   // no archive yet: nothing to find
    if (!S_ISDIR(rst.st_mode)) { ix.error = "~/Archive isn't a folder"; return ix; }

    // What the catalog knows (read-only), by path.
    struct Known { std::string kind; std::int64_t read = 0; };
    std::unordered_map<std::string, Known> known;
    const auto rows = catalog::query(data_dir,
        "SELECT path, coalesce(kind, ''), coalesce(last_read, 0) FROM files "
        "WHERE coalesce(gone, 0) = 0 AND path GLOB 'Archive/*'", {}, kIndexCap);
    if (rows.ok)
        for (const auto& r : rows.rows)
            if (r.size() == 3) {
                Known k;
                k.kind = r[1];
                try { k.read = std::stoll(r[2]); } catch (...) {}
                known.emplace(r[0], std::move(k));
            }
    // Rouster's records, by where each item sits now.
    std::unordered_map<std::string, std::size_t> by_rel;
    for (std::size_t i = 0; i < stored.size(); ++i) by_rel.emplace(stored[i].rel, i);

    std::set<int> years;
    const auto add = [&](const std::string& rel, const std::string& name, bool package, const struct stat& st,
                         std::uint64_t bytes, std::int64_t newest) {
        Entry e;
        e.rel = rel;
        e.name = name;
        e.package = package;
        e.ext = ext_of(name);
        e.bytes = bytes;
        e.changed = newest ? newest : static_cast<std::int64_t>(st.st_mtime);
        // Archive/<year>/...
        const std::string after = rel.substr(std::string(kRoot).size() + 1);
        const auto slash = after.find('/');
        const std::string first = after.substr(0, slash);
        if (slash != std::string::npos && first.size() == 4 && std::all_of(first.begin(), first.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
            e.year = std::stoi(first);
            e.from = after.substr(slash + 1);
        }
        // The nearest record: the item itself, or the archived folder it's in.
        for (std::string p = rel;;) {
            if (auto it = by_rel.find(p); it != by_rel.end()) {
                const Stored& s = stored[it->second];
                e.stored = it->second;
                e.inside = p != rel;
                e.recorded = true;
                e.from = s.back + rel.substr(p.size());
                e.archived = s.when;
                break;
            }
            const auto k = p.rfind('/');
            if (k == std::string::npos || k <= std::string(kRoot).size()) break;
            p.resize(k);
        }
        std::string ct;
        if (auto it = known.find(rel); it != known.end()) { ct = it->second.kind; e.read = it->second.read; }
        e.kind = search_kind(e.ext, ct);
        if (e.year) years.insert(e.year);
        ix.bytes += e.bytes;
        ix.entries.push_back(std::move(e));
    };

    // A package: one entry -- its size all of it, its change the newest inside.
    const auto measure = [&](const std::string& abs, std::uint64_t& bytes, std::int64_t& newest) {
        struct stat st{};
        for (auto it = fs::recursive_directory_iterator(abs, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (::lstat(it->path().c_str(), &st) == 0) {
                bytes += static_cast<std::uint64_t>(st.st_blocks) * 512u;
                if (S_ISREG(st.st_mode)) newest = std::max<std::int64_t>(newest, st.st_mtime);
            }
        ec.clear();
    };

    std::vector<std::string> todo = {kRoot};
    while (!todo.empty() && !ix.truncated) {
        const std::string rel = todo.back();
        todo.pop_back();
        DIR* d = ::opendir((home + "/" + rel).c_str());
        if (!d) { ++ix.unread; continue; }
        std::vector<std::string> names;
        while (dirent* de = ::readdir(d)) {
            const std::string n = de->d_name;
            if (n != "." && n != ".." && n[0] != '.') names.push_back(n);
        }
        ::closedir(d);
        std::sort(names.begin(), names.end(), std::greater<>());   // popped back to front: a-z
        for (const auto& n : names) {
            const std::string crel = rel + "/" + n, cabs = home + "/" + crel;
            struct stat st{};
            if (::lstat(cabs.c_str(), &st) != 0) { ++ix.unread; continue; }
            if (S_ISDIR(st.st_mode)) {
                if (st.st_dev != rst.st_dev) continue;   // another filesystem: not walked
                if (pkg::is_package_name(n)) {
                    std::uint64_t b = 0;
                    std::int64_t newest = 0;
                    measure(cabs, b, newest);
                    add(crel, n, true, st, b, newest);
                } else {
                    todo.push_back(crel);
                }
            } else if (S_ISREG(st.st_mode)) {
                add(crel, n, false, st, static_cast<std::uint64_t>(st.st_blocks) * 512u, 0);
            }
            if (ix.entries.size() >= kIndexCap) { ix.truncated = true; break; }
        }
    }
    std::sort(ix.entries.begin(), ix.entries.end(), [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
    ix.years.assign(years.rbegin(), years.rend());
    ix.ok = true;
    return ix;
}

std::vector<std::size_t> match(const Index& ix, const Query& q, std::size_t limit, std::size_t* total) {
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : q.text + " ") {
            if (std::isspace(static_cast<unsigned char>(c))) { if (!w.empty()) words.push_back(lower(w)); w.clear(); }
            else w += c;
        }
    }
    struct Hit { std::size_t i; bool by_name; std::int64_t changed; };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < ix.entries.size(); ++i) {
        const Entry& e = ix.entries[i];
        if (!q.kind.empty() && e.kind != q.kind) continue;
        if (q.year && e.year != q.year) continue;
        const std::string name = lower(e.name);
        const std::string where = lower(e.from.empty() ? e.rel : e.from + "\n" + e.rel);
        bool all = true, by_name = true;
        for (const auto& w : words) {
            if (name.find(w) != std::string::npos) continue;
            by_name = false;
            if (where.find(w) == std::string::npos) { all = false; break; }
        }
        if (all) hits.push_back({i, by_name, e.changed});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.by_name != b.by_name) return a.by_name;
        return a.changed > b.changed;
    });
    if (total) *total = hits.size();
    std::vector<std::size_t> out;
    for (std::size_t k = 0; k < hits.size() && k < limit; ++k) out.push_back(hits[k].i);
    return out;
}

}  // namespace rouster::core::archive
