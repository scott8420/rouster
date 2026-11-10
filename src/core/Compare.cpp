// Compare (s017b) -- see include/core/Compare.hpp. READ-ONLY: files are opened
// to be read; nothing here writes.
#include "core/Compare.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <unordered_map>

#include <sys/stat.h>

namespace rouster::core::compare {

namespace {

bool in(const std::string& ext, std::initializer_list<const char*> list) {
    for (const char* e : list)
        if (ext == e) return true;
    return false;
}

// Read at most `cap` bytes from the start of a regular file; `more` = there was more.
bool slurp(const std::string& path, std::uint64_t cap, std::string& out, bool* more) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(static_cast<std::size_t>(cap) + 1, '\0');
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    const auto got = static_cast<std::size_t>(f.gcount());
    if (more) *more = got > cap;
    out.resize(std::min<std::size_t>(got, static_cast<std::size_t>(cap)));
    return true;
}

std::uint32_t le16(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8); }
std::uint32_t le32(const unsigned char* p) { return le16(p) | (le16(p + 2) << 16); }

bool read_at(std::ifstream& f, std::uint64_t off, std::size_t n, std::string& out) {
    out.assign(n, '\0');
    f.clear();
    f.seekg(static_cast<std::streamoff>(off));
    if (!f) return false;
    f.read(out.data(), static_cast<std::streamsize>(n));
    return static_cast<std::size_t>(f.gcount()) == n;
}

// "&amp;" etc. at s[i] ('&'); appends the character(s) and returns the length eaten (0 = not an entity).
std::size_t entity(const std::string& s, std::size_t i, std::string& out) {
    const auto semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) return 0;
    const std::string e = s.substr(i + 1, semi - i - 1);
    std::uint32_t cp = 0;
    if (e == "amp") cp = '&';
    else if (e == "lt") cp = '<';
    else if (e == "gt") cp = '>';
    else if (e == "quot") cp = '"';
    else if (e == "apos") cp = '\'';
    else if (e.size() > 1 && e[0] == '#') {
        try {
            cp = static_cast<std::uint32_t>(e[1] == 'x' || e[1] == 'X' ? std::stoul(e.substr(2), nullptr, 16) : std::stoul(e.substr(1)));
        } catch (...) { return 0; }
    } else return 0;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return semi - i + 1;
}

struct Tag {
    std::string name;            // "text:p"
    bool        closing = false, empty = false;
    std::string attrs;
};
// s[i] == '<'. Fills `t`; returns the index after '>' (s.size() if unterminated). Comments / PIs: name "".
std::size_t tag_at(const std::string& s, std::size_t i, Tag& t) {
    t = Tag{};
    if (s.compare(i, 4, "<!--") == 0) {
        const auto e = s.find("-->", i + 4);
        return e == std::string::npos ? s.size() : e + 3;
    }
    const auto e = s.find('>', i);
    if (e == std::string::npos) return s.size();
    if (s[i + 1] == '?' || s[i + 1] == '!') return e + 1;
    std::size_t p = i + 1;
    if (s[p] == '/') { t.closing = true; ++p; }
    std::size_t q = p;
    while (q < e && !std::isspace(static_cast<unsigned char>(s[q])) && s[q] != '/') ++q;
    t.name = s.substr(p, q - p);
    t.empty = e > i && s[e - 1] == '/';
    t.attrs = s.substr(q, e - q);
    return e + 1;
}

std::size_t attr_count(const std::string& attrs, const char* name) {   // text:c="3" -> 3 (default 1)
    const auto k = attrs.find(name);
    if (k == std::string::npos) return 1;
    const auto q = attrs.find_first_of("\"'", k);
    if (q == std::string::npos) return 1;
    try { return std::clamp<std::size_t>(std::stoul(attrs.substr(q + 1)), 1, 1000); } catch (...) { return 1; }
}

}  // namespace

std::string clean_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    static const char kBad[] = "\xEF\xBF\xBD";
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c == 0) { out += kBad; ++i; continue; }
        if (c < 0x80) { out += static_cast<char>(c); ++i; continue; }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        bool good = len && i + len <= n;
        for (std::size_t k = 1; good && k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) good = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (good) {   // no overlongs, no surrogates, nothing past U+10FFFF
            const std::uint32_t min = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
            good = cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        }
        if (good) { out.append(s, i, len); i += len; }
        else { out += kBad; ++i; }
    }
    return out;
}

std::string app_of(const std::string& ext) {
    if (ext == "curvz") return "Curvz";
    if (ext == "folio") return "Folio";
    if (ext == "jots") return "jot";
    if (ext == "optik") return "Optik";
    return "";
}

Kind kind_of(const std::string& path, const std::string& ext, bool package) {
    if (!app_of(ext).empty()) return Kind::App;
    if (package) return Kind::Other;
    if (in(ext, {"png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff", "svg", "ico", "avif", "heic", "heif", "tga"}))
        return Kind::Image;
    if (in(ext, {"odt", "ott", "fodt", "docx", "txt", "text", "md", "markdown", "rst", "org", "csv", "tsv", "json", "xml",
                 "html", "htm", "css", "js", "ts", "py", "sh", "c", "h", "cc", "cpp", "hpp", "cxx", "rs", "go", "java",
                 "ini", "conf", "cfg", "toml", "yaml", "yml", "log", "tex", "srt", "rtf", "fountain"}))
        return Kind::Text;
    // Not a known kind: a small file that reads as text is one.
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > kTextCap) return Kind::Other;
    std::string head;
    if (!slurp(path, 8192, head, nullptr) || head.empty()) return Kind::Other;
    if (head.find('\0') != std::string::npos) return Kind::Other;
    // Cut at a character boundary so a split last character isn't counted bad.
    std::size_t end = head.size();
    for (std::size_t k = 0; k < 3 && end > 0 && (static_cast<unsigned char>(head[end - 1]) & 0xC0) == 0x80; ++k) --end;
    if (end > 0 && static_cast<unsigned char>(head[end - 1]) >= 0xC0) --end;
    head.resize(end);
    const std::string clean = clean_utf8(head);
    const std::size_t bad = (clean.size() - head.size()) / 2;   // each bad byte grows by 2
    return bad * 100 <= head.size() ? Kind::Text : Kind::Other;
}

std::string zip_member(const std::string& zip_path, const std::string& name, std::uint64_t cap, std::string* why) {
    const auto fail = [&](const char* w) { if (why) *why = w; return std::string(); };
    std::ifstream f(zip_path, std::ios::binary);
    if (!f) return fail("can't be opened");
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::uint64_t>(f.tellg());
    if (size < 22) return fail("not a zip");
    const std::uint64_t tail = std::min<std::uint64_t>(size, 22 + 65535);
    std::string t;
    if (!read_at(f, size - tail, static_cast<std::size_t>(tail), t)) return fail("can't be read");
    std::size_t eocd = std::string::npos;
    for (std::size_t k = t.size() - 22 + 1; k-- > 0;)
        if (le32(reinterpret_cast<const unsigned char*>(t.data()) + k) == 0x06054b50) { eocd = k; break; }
    if (eocd == std::string::npos) return fail("not a zip");
    const auto* e = reinterpret_cast<const unsigned char*>(t.data()) + eocd;
    const std::uint64_t cd_size = le32(e + 12), cd_off = le32(e + 16);
    if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) return fail("a zip64 -- too big to read here");
    if (cd_off + cd_size > size || cd_size > (64u << 20)) return fail("a damaged zip");
    std::string cd;
    if (!read_at(f, cd_off, static_cast<std::size_t>(cd_size), cd)) return fail("a damaged zip");
    const auto* c = reinterpret_cast<const unsigned char*>(cd.data());
    for (std::size_t p = 0; p + 46 <= cd.size();) {
        if (le32(c + p) != 0x02014b50) return fail("a damaged zip");
        const std::uint32_t method = le16(c + p + 10), csize = le32(c + p + 20), usize = le32(c + p + 24);
        const std::uint32_t nlen = le16(c + p + 28), xlen = le16(c + p + 30), klen = le16(c + p + 32), loff = le32(c + p + 42);
        if (p + 46 + nlen > cd.size()) return fail("a damaged zip");
        if (cd.compare(p + 46, nlen, name) == 0 && nlen == name.size()) {
            if (usize > cap) return fail("too big");
            if (method != 0 && method != 8) return fail("compressed in a way Rouster doesn't read");
            std::string lh;
            if (!read_at(f, loff, 30, lh) || le32(reinterpret_cast<const unsigned char*>(lh.data())) != 0x04034b50)
                return fail("a damaged zip");
            const auto* l = reinterpret_cast<const unsigned char*>(lh.data());
            const std::uint64_t data = static_cast<std::uint64_t>(loff) + 30 + le16(l + 26) + le16(l + 28);
            std::string comp;
            if (data + csize > size || !read_at(f, data, csize, comp)) return fail("a damaged zip");
            if (method == 0) {
                if (comp.size() != usize) return fail("a damaged zip");
                return comp;
            }
            std::string out(usize, '\0');
            z_stream z{};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return fail("can't be inflated");
            z.next_in = reinterpret_cast<Bytef*>(comp.data());
            z.avail_in = static_cast<uInt>(comp.size());
            z.next_out = reinterpret_cast<Bytef*>(out.data());
            z.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&z, Z_FINISH);
            const auto produced = z.total_out;
            inflateEnd(&z);
            if (rc != Z_STREAM_END || produced != usize) return fail("a damaged zip");
            return out;
        }
        p += 46 + nlen + xlen + klen;
    }
    if (why) *why = "the zip has no " + name;
    return {};
}

std::vector<std::string> odf_lines(const std::string& xml) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = xml.find("<office:body");
    if (i == std::string::npos) i = 0;
    int skip = 0;               // inside an annotation / tracked changes / a note
    bool space = false;         // the last raw character was collapsed whitespace
    const auto push = [&] { lines.push_back(line); line.clear(); space = false; };
    while (i < xml.size() && lines.size() <= kLineCap) {
        if (xml[i] == '<') {
            Tag t;
            i = tag_at(xml, i, t);
            if (t.name.empty()) continue;
            const bool skipper = t.name == "office:annotation" || t.name == "text:tracked-changes" || t.name == "text:note";
            if (skipper) {
                if (!t.empty) skip = std::max(0, skip + (t.closing ? -1 : 1));
                continue;
            }
            if (skip) continue;
            if (t.name == "text:p" || t.name == "text:h") {
                if (t.closing || t.empty) push();
                else if (!line.empty()) push();   // a paragraph opened inside another (a frame): its own line
            } else if (t.name == "text:line-break") push();
            else if (t.name == "text:tab") { line += '\t'; space = false; }
            else if (t.name == "text:s") { line.append(attr_count(t.attrs, "text:c"), ' '); space = false; }
            continue;
        }
        if (skip) { ++i; continue; }
        const char ch = xml[i];
        if (ch == '&') {
            std::string e;
            const std::size_t n = entity(xml, i, e);
            if (n) { line += e; space = false; i += n; continue; }
        }
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            if (!space) line += ' ';
            space = true;
        } else {
            line += ch;
            space = false;
        }
        ++i;
    }
    if (!line.empty()) push();
    for (auto& l : lines) l = clean_utf8(l);
    return lines;
}

std::vector<std::string> docx_lines(const std::string& xml) {
    std::vector<std::string> lines;
    std::string line;
    bool text = false;          // inside <w:t>
    std::size_t i = xml.find("<w:body");
    if (i == std::string::npos) i = 0;
    while (i < xml.size() && lines.size() <= kLineCap) {
        if (xml[i] == '<') {
            Tag t;
            i = tag_at(xml, i, t);
            if (t.name == "w:t") text = !t.closing && !t.empty;
            else if (t.name == "w:p" && (t.closing || t.empty)) { lines.push_back(line); line.clear(); }
            else if ((t.name == "w:br" || t.name == "w:cr") && !t.closing) { lines.push_back(line); line.clear(); }
            else if (t.name == "w:tab" && !t.closing) line += '\t';
            continue;
        }
        if (text) {
            std::string e;
            const std::size_t n = xml[i] == '&' ? entity(xml, i, e) : 0;
            if (n) { line += e; i += n; continue; }
            line += xml[i];
        }
        ++i;
    }
    if (!line.empty()) lines.push_back(line);
    for (auto& l : lines) l = clean_utf8(l);
    return lines;
}

Text read_text(const std::string& path, const std::string& ext) {
    Text t;
    std::string why;
    if (ext == "odt" || ext == "ott" || ext == "docx") {
        const bool docx = ext == "docx";
        const std::string xml = zip_member(path, docx ? "word/document.xml" : "content.xml", kTextCap, &why);
        if (xml.empty()) { t.why = why.empty() ? "empty" : why; return t; }
        t.lines = docx ? docx_lines(xml) : odf_lines(xml);
    } else {
        std::string bytes;
        bool more = false;
        if (!slurp(path, kTextCap, bytes, &more)) { t.why = "can't be opened"; return t; }
        if (bytes.substr(0, 8192).find('\0') != std::string::npos) { t.why = "not a text"; return t; }
        t.cut = more;
        if (ext == "fodt") t.lines = odf_lines(bytes);
        else {
            if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);   // a BOM
            std::size_t a = 0;
            while (a <= bytes.size() && t.lines.size() <= kLineCap) {
                auto b = bytes.find('\n', a);
                if (b == std::string::npos) b = bytes.size();
                std::string l = bytes.substr(a, b - a);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                if (b == bytes.size() && l.empty()) break;   // the last newline isn't a line
                t.lines.push_back(clean_utf8(l));
                a = b + 1;
            }
        }
    }
    if (t.lines.size() > kLineCap) { t.lines.resize(kLineCap); t.cut = true; }
    t.ok = true;
    return t;
}

// ── the diff ─────────────────────────────────────────────────────────────────
namespace {

enum class Op : std::uint8_t { Eq, Del, Ins };

// Myers' greedy O((N+M)D) diff over ids; false if D would pass `budget`.
bool myers(const std::vector<int>& a, const std::vector<int>& b, std::size_t budget, std::vector<Op>& ops) {
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    const int max = static_cast<int>(std::min<std::size_t>(a.size() + b.size(), budget));
    const int off = max + 1;
    std::vector<int> v(static_cast<std::size_t>(2 * off + 1), 0);
    // Each round's frontier, only the slice it can touch (k in [-d-1, d+1]):
    // memory ~ D^2 ints, so the budget bounds it (2000 -> ~16 MB at worst).
    std::vector<std::vector<int>> trace;
    const auto V = [&](int k) -> int& { return v[static_cast<std::size_t>(off + k)]; };
    int found = -1;
    for (int d = 0; d <= max; ++d) {
        trace.emplace_back(v.begin() + (off - d - 1 < 0 ? 0 : off - d - 1), v.begin() + std::min(2 * off + 1, off + d + 2));
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && V(k - 1) < V(k + 1))) ? V(k + 1) : V(k - 1) + 1;
            int y = x - k;
            while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) { ++x; ++y; }
            V(k) = x;
            if (x >= n && y >= m) { found = d; break; }
        }
        if (found >= 0) break;
    }
    if (found < 0) return false;
    // Walk back through the saved frontiers.
    std::vector<Op> rev;
    int x = n, y = m;
    for (int d = found; d > 0; --d) {
        const auto& pv = trace[static_cast<std::size_t>(d)];
        const int base = off - d - 1 < 0 ? 0 : off - d - 1;
        const auto P = [&](int k) { return pv[static_cast<std::size_t>(off + k - base)]; };
        const int k = x - y;
        const int pk = (k == -d || (k != d && P(k - 1) < P(k + 1))) ? k + 1 : k - 1;
        const int px = P(pk), py = px - pk;
        while (x > px && y > py) { rev.push_back(Op::Eq); --x; --y; }
        rev.push_back(x == px ? Op::Ins : Op::Del);
        x = px;
        y = py;
    }
    while (x > 0 && y > 0) { rev.push_back(Op::Eq); --x; --y; }
    ops.assign(rev.rbegin(), rev.rend());
    return true;
}

}  // namespace

Rows rows(const std::vector<std::string>& left, const std::vector<std::string>& right, std::size_t context) {
    Rows out;
    // Lines -> ids, so the diff compares ints.
    std::unordered_map<std::string, int> ids;
    const auto id = [&](const std::string& s) { return ids.emplace(s, static_cast<int>(ids.size())).first->second; };
    std::vector<int> a, b;
    a.reserve(left.size());
    b.reserve(right.size());
    for (const auto& s : left) a.push_back(id(s));
    for (const auto& s : right) b.push_back(id(s));
    // The same head and tail need no search.
    std::size_t head = 0;
    while (head < a.size() && head < b.size() && a[head] == b[head]) ++head;
    std::size_t tail = 0;
    while (tail < a.size() - head && tail < b.size() - head && a[a.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    std::vector<int> ma(a.begin() + static_cast<long>(head), a.end() - static_cast<long>(tail));
    std::vector<int> mb(b.begin() + static_cast<long>(head), b.end() - static_cast<long>(tail));
    std::vector<Op> mid;
    std::vector<Row> all;
    if (!myers(ma, mb, kEditBudget, mid)) {
        out.too_different = true;
        const std::size_t n = std::max(left.size(), right.size());
        for (std::size_t k = 0; k < n; ++k) {
            Row r;
            if (k < left.size()) { r.left = left[k]; r.lno = k + 1; }
            if (k < right.size()) { r.right = right[k]; r.rno = k + 1; }
            all.push_back(std::move(r));
        }
        out.rows = std::move(all);
        return out;
    }
    std::vector<Op> ops(head, Op::Eq);
    ops.insert(ops.end(), mid.begin(), mid.end());
    ops.insert(ops.end(), tail, Op::Eq);

    std::size_t i = 0, j = 0;
    std::vector<std::size_t> dels, inss;
    const auto flush = [&] {
        const std::size_t pair = std::min(dels.size(), inss.size());
        for (std::size_t k = 0; k < pair; ++k) {
            all.push_back(Row{Mark::Changed, left[dels[k]], right[inss[k]], dels[k] + 1, inss[k] + 1, 0});
            ++out.changed;
        }
        for (std::size_t k = pair; k < dels.size(); ++k) { all.push_back(Row{Mark::Removed, left[dels[k]], "", dels[k] + 1, 0, 0}); ++out.removed; }
        for (std::size_t k = pair; k < inss.size(); ++k) { all.push_back(Row{Mark::Added, "", right[inss[k]], 0, inss[k] + 1, 0}); ++out.added; }
        dels.clear();
        inss.clear();
    };
    for (const Op op : ops) {
        if (op == Op::Eq) {
            flush();
            all.push_back(Row{Mark::Same, left[i], right[j], i + 1, j + 1, 0});
            ++out.same;
            ++i; ++j;
        } else if (op == Op::Del) dels.push_back(i++);
        else inss.push_back(j++);
    }
    flush();

    if (!context) { out.rows = std::move(all); return out; }
    // Fold long same runs: keep `context` lines next to a change.
    for (std::size_t p = 0; p < all.size();) {
        if (all[p].mark != Mark::Same) { out.rows.push_back(std::move(all[p++])); continue; }
        std::size_t q = p;
        while (q < all.size() && all[q].mark == Mark::Same) ++q;
        const std::size_t run = q - p;
        const std::size_t keep_head = p == 0 ? 0 : context;          // after a change
        const std::size_t keep_tail = q == all.size() ? 0 : context; // before a change
        if (run > keep_head + keep_tail + 1) {
            for (std::size_t k = p; k < p + keep_head; ++k) out.rows.push_back(std::move(all[k]));
            Row gap;
            gap.mark = Mark::Gap;
            gap.skipped = run - keep_head - keep_tail;
            out.rows.push_back(std::move(gap));
            for (std::size_t k = q - keep_tail; k < q; ++k) out.rows.push_back(std::move(all[k]));
        } else {
            for (std::size_t k = p; k < q; ++k) out.rows.push_back(std::move(all[k]));
        }
        p = q;
    }
    return out;
}

}  // namespace rouster::core::compare
