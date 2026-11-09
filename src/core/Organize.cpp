#include "core/Organize.hpp"
#include "core/Package.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <sys/stat.h>

// Organize.cpp -- READ-ONLY. It reads templates and projects and returns
// plans; nothing here writes (the selftest's source gate holds it to that).

namespace rouster::core::organize {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool exists(const std::string& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0; }
bool is_dir(const std::string& p) { struct stat st{}; return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void replace_all(std::string& s, const std::string& a, const std::string& b) {
    for (std::size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
}

std::string ext_of(const std::string& name) {
    const auto d = name.rfind('.');
    return d == std::string::npos || d == 0 || d + 1 == name.size() ? std::string() : lower(name.substr(d + 1));
}
std::string stem_of(const std::string& name) {
    const auto d = name.rfind('.');
    return d == std::string::npos || d == 0 || d + 1 == name.size() ? name : name.substr(0, d);
}

bool read_small(const std::string& path, std::string& out, std::size_t max) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(max + 1, '\0');
    in.read(out.data(), static_cast<std::streamsize>(max + 1));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out.size() <= max;
}

const std::vector<std::string>& tokens() {
    static const std::vector<std::string> t = {"{id}", "{title}", "{date}", "{year}", "{yyyymmdd}", "{yyyymmdd-hhmm}"};
    return t;
}

Rules rules_from(const json& j) {
    Rules r;
    r.pattern = j.value("pattern", "");
    r.versions = j.value("versions", "versions");
    if (r.versions.empty()) r.versions = "versions";
    if (j.contains("routes") && j["routes"].is_object())
        for (auto it = j["routes"].begin(); it != j["routes"].end(); ++it) {
            Route rt{it.key(), {}};
            if (it.value().is_array())
                for (const auto& e : it.value())
                    if (e.is_string()) rt.exts.push_back(lower(e.get<std::string>()));
            r.routes.push_back(std::move(rt));
        }
    return r;
}

json rules_json(const Rules& r) {
    json routes = json::object();
    for (const auto& rt : r.routes) routes[rt.folder] = rt.exts;
    return json{{"pattern", r.pattern}, {"versions", r.versions}, {"routes", routes}};
}

std::string regex_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (std::strchr(".^$|()[]{}*+?\\/", c) && c != '\0') o += '\\';
        o += c;
    }
    return o;
}

door::Action make_dir(const std::string& p, std::string why) {
    door::Action a;
    a.verb = door::Verb::MakeDir;
    a.to = p;
    a.reason = std::move(why);
    return a;
}

// MakeDir for every missing folder from `base` (which exists) down to `dir`.
void make_path(const std::string& base, const std::string& dir, std::vector<std::string>& made,
               std::vector<door::Action>& out, const std::string& why) {
    if (dir == base || dir.rfind(base + "/", 0) != 0) return;
    std::string cur = base;
    const std::string rest = dir.substr(base.size() + 1);
    std::size_t pos = 0;
    while (pos <= rest.size()) {
        const auto s = rest.find('/', pos);
        cur = base + "/" + rest.substr(0, s);
        if (!exists(cur) && std::find(made.begin(), made.end(), cur) == made.end()) {
            made.push_back(cur);
            out.push_back(make_dir(cur, why));
        }
        if (s == std::string::npos) break;
        pos = s + 1;
    }
}

}  // namespace

std::string slug(const std::string& s) {
    static const std::string bad = " _/\\:*?\"<>|{}-\t\r\n";
    std::string out;
    bool dash = false;
    for (unsigned char c : s) {
        if (c < 0x20 || bad.find(static_cast<char>(c)) != std::string::npos) { dash = true; continue; }
        if (dash && !out.empty()) out += '-';
        dash = false;
        out += static_cast<char>(c);
    }
    while (!out.empty() && out[0] == '.') out.erase(0, 1);   // never a hidden name
    return out;
}

std::string stamp(std::int64_t when, bool minutes) {
    std::time_t t = static_cast<std::time_t>(when);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, minutes ? "%Y%m%d-%H%M" : "%Y%m%d", &tm);
    return b;
}

std::string fill(const std::string& s, const Values& v, bool names) {
    std::string o = s;
    std::time_t t = static_cast<std::time_t>(v.when);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char date[16], year[8];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    std::strftime(year, sizeof year, "%Y", &tm);
    replace_all(o, "{id}", names ? slug(v.id) : v.id);
    replace_all(o, "{title}", names ? slug(v.title) : v.title);
    replace_all(o, "{date}", date);
    replace_all(o, "{year}", year);
    replace_all(o, "{yyyymmdd-hhmm}", stamp(v.when, true));
    replace_all(o, "{yyyymmdd}", stamp(v.when, false));
    return o;
}

// ── templates ───────────────────────────────────────────────────────────────
std::vector<Template> templates(const std::string& home) {
    std::vector<Template> out;
    const std::string dir = home + "/" + kTemplates;
    std::error_code ec;
    if (!is_dir(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.' || !is_dir(e.path().string())) continue;
        Template t;
        t.name = name;
        t.path = e.path().string();
        const std::string meta = t.path + "/" + kMeta;
        if (exists(meta)) {
            std::ifstream in(meta);
            json j = json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_object()) t.problem = std::string(kMeta) + " is not valid JSON";
            else {
                t.root = j.value("root", t.root);
                t.folder = j.value("folder", t.folder);
                t.rules = rules_from(j);
                while (!t.root.empty() && t.root.back() == '/') t.root.pop_back();
                if (t.root.empty() || t.root[0] == '/' || t.root.find("..") != std::string::npos)
                    t.problem = "\"root\" must be a folder under home";
                if (t.folder.empty() || t.folder.find('/') != std::string::npos)
                    t.problem = "\"folder\" must be one name";
            }
        }
        out.push_back(std::move(t));
    }
    std::sort(out.begin(), out.end(), [](const Template& a, const Template& b) { return a.name < b.name; });
    return out;
}

// The starters. Q7 is open: the Design job's pattern is the one in §4.7 until
// Scott's own design-job pattern replaces it (edit its .rouster.json).
namespace {
struct Starter {
    const char* name;
    json meta;
    std::vector<std::string> dirs;
    std::vector<std::pair<std::string, std::string>> files;   // rel, text
};
std::vector<Starter> starter_set() {
    const std::vector<std::string> fonts = {"ttf", "otf", "woff", "woff2", "pfb", "pfm", "afm", "ttc", "dfont"};
    std::vector<Starter> s;
    s.push_back({"Design job",
                 json{{"kind", "template"}, {"root", "Documents/Jobs"}, {"folder", "{id}_{title}"},
                      {"pattern", "{id}_{title}_{yyyymmdd-hhmm}_{part}.{ext}"}, {"versions", "versions"},
                      {"routes", {{"brief", {"doc", "docx", "odt", "rtf", "txt", "md", "eml"}},
                                  {"fonts", fonts},
                                  {"images", {"jpg", "jpeg", "png", "tif", "tiff", "gif", "webp", "heic", "bmp",
                                              "raw", "cr2", "cr3", "nef", "arw", "dng", "eps"}},
                                  {"workups", {"psd", "ai", "indd", "idml", "svg", "xcf", "kra", "afdesign",
                                               "afphoto", "sketch", "fig", "sla"}},
                                  {"proofs", {"pdf"}}}}},
                 {"brief", "fonts", "images", "workups", "proofs", "finals", "versions"},
                 {{"brief/{id}_{title}_{yyyymmdd-hhmm}_brief.md",
                   "# {title}\n\nJob {id} -- opened {date}.\n\nClient:\nContact:\nDue:\n\nThe brief:\n\n"}}});
    s.push_back({"Book",
                 json{{"kind", "template"}, {"root", "Documents/Books"}, {"folder", "{id}_{title}"},
                      {"pattern", "{id}_{title}_{yyyymmdd-hhmm}_{part}.{ext}"}, {"versions", "versions"},
                      {"routes", {{"manuscript", {"doc", "docx", "odt", "rtf", "txt", "md", "fdx", "scriv"}},
                                  {"cover", {"psd", "ai", "svg", "jpg", "jpeg", "png", "tif", "tiff", "xcf", "kra"}},
                                  {"imposition", {"indd", "idml", "sla"}},
                                  {"fonts", fonts},
                                  {"proofs", {"pdf", "epub", "mobi"}}}}},
                 {"manuscript", "cover", "imposition", "fonts", "proofs", "versions"},
                 {{"manuscript/{id}_{title}_{yyyymmdd-hhmm}_notes.md",
                   "# {title}\n\nBook {id} -- begun {date}.\n\nLogline:\n\nCharacters:\n\nNotes:\n"}}});
    s.push_back({"gtkmm app",
                 json{{"kind", "template"}, {"root", "Develop"}, {"folder", "{id}"}, {"pattern", ""},
                      {"versions", "versions"}, {"routes", json::object()}},
                 {"docs", "scripts", "backups", "backups/Old", "{id}", "{id}/include", "{id}/src",
                  "{id}/resources", "{id}/data"},
                 {{"docs/NOTES.md", "# {title}\n\nStarted {date}. The docs live here, outside the repo: RULES, "
                                    "CANON, ARCHITECTURE, ARC, PROGRESS, HANDOFF.\n"},
                  {"{id}/README.md", "# {title}\n\nA gtkmm 4 / C++23 app.\n"}}});
    return s;
}
}  // namespace

door::Plan starters(const std::string& home, std::vector<std::string>& skipped) {
    door::Plan plan;
    plan.label = "Starter templates";
    std::vector<std::string> made;
    const std::string dir = home + "/" + kTemplates;
    make_path(home, dir, made, plan.actions, "where Rouster's templates live");
    for (const auto& s : starter_set()) {
        const std::string root = dir + "/" + s.name;
        if (exists(root)) { skipped.push_back(s.name); continue; }
        plan.actions.push_back(make_dir(root, std::string("the ") + s.name + " template"));
        for (const auto& d : s.dirs) plan.actions.push_back(make_dir(root + "/" + d, "a folder every project gets"));
        door::Action m;
        m.verb = door::Verb::Write;
        m.to = root + "/" + kMeta;
        m.content = s.meta.dump(2) + "\n";
        m.reason = "where its projects go, their folder name, the naming pattern, the routes";
        m.bytes = m.content.size();
        plan.actions.push_back(m);
        for (const auto& [rel, text] : s.files) {
            door::Action w;
            w.verb = door::Verb::Write;
            w.to = root + "/" + rel;
            w.content = text;
            w.reason = "a starting file; its placeholders are filled in for each project";
            w.bytes = text.size();
            plan.actions.push_back(w);
        }
    }
    return plan;
}

// ── projects ────────────────────────────────────────────────────────────────
std::string project_dir(const std::string& home, const Template& t, const Values& v) {
    return home + "/" + t.root + "/" + fill(t.folder, v);
}

door::Plan new_project(const std::string& home, const Template& t, const Values& v, std::vector<std::string>& problems) {
    door::Plan plan;
    if (!t.problem.empty()) { problems.push_back(t.name + ": " + t.problem); return plan; }
    const auto uses = [&](const char* tok) {
        return t.folder.find(tok) != std::string::npos || t.rules.pattern.find(tok) != std::string::npos;
    };
    if (uses("{id}") && slug(v.id).empty()) problems.push_back("an id is needed");
    if (uses("{title}") && slug(v.title).empty()) problems.push_back("a title is needed");
    const std::string folder = fill(t.folder, v);
    if (slug(folder).empty()) problems.push_back("the folder name comes out empty");
    if (!problems.empty()) return plan;
    const std::string dir = project_dir(home, t, v);
    if (exists(dir)) { problems.push_back("already there: " + dir); return plan; }
    if (!is_dir(t.path)) { problems.push_back("the template is gone: " + t.path); return plan; }

    plan.label = "New project: " + folder;
    std::vector<std::string> made;
    make_path(home, fs::path(dir).parent_path().string(), made, plan.actions, "where " + t.name + " projects go");
    made.push_back(dir);
    plan.actions.push_back(make_dir(dir, "the project"));

    // The template's tree, parents before children.
    std::vector<std::pair<std::string, bool>> entries;   // rel, is dir
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(t.path, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string rel = fs::relative(it->path(), t.path, ec).string();
        const std::string name = it->path().filename().string();
        if (rel == kMeta || name.rfind(".rouster-", 0) == 0) continue;
        struct stat st{};
        if (::lstat(it->path().c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) entries.emplace_back(rel, true);
        else if (S_ISREG(st.st_mode)) entries.emplace_back(rel, false);
        else if (S_ISLNK(st.st_mode)) it.disable_recursion_pending();   // links are left out
    }
    std::sort(entries.begin(), entries.end());
    for (const auto& [rel, isdir] : entries) {
        const std::string to = dir + "/" + fill(rel, v);
        if (isdir) { plan.actions.push_back(make_dir(to, "from the template")); continue; }
        const std::string src = t.path + "/" + rel;
        std::string body;
        bool text = read_small(src, body, 64 * 1024) && body.find('\0') == std::string::npos;
        bool has = false;
        for (const auto& tk : tokens()) if (body.find(tk) != std::string::npos) has = true;
        door::Action a;
        a.to = to;
        if (text && has) {
            a.verb = door::Verb::Write;
            a.content = fill(body, v, false);
            a.bytes = a.content.size();
            a.reason = "from the template, placeholders filled in";
        } else {
            struct stat st{};
            ::lstat(src.c_str(), &st);
            a.verb = door::Verb::Copy;
            a.from = src;
            a.bytes = static_cast<std::uint64_t>(st.st_size);
            a.reason = "copied from the template";
        }
        plan.actions.push_back(a);
    }
    door::Action m;
    m.verb = door::Verb::Write;
    m.to = dir + "/" + kMeta;
    json j = rules_json(t.rules);
    j["kind"] = "project";
    j["template"] = t.name;
    j["id"] = v.id;
    j["title"] = v.title;
    j["created"] = v.when;
    m.content = j.dump(2) + "\n";
    m.bytes = m.content.size();
    m.reason = "the project's own naming rules (a later template edit changes no project)";
    plan.actions.push_back(m);
    return plan;
}

bool load_project(const std::string& dir, Project& out) {
    std::ifstream in(dir + "/" + kMeta);
    if (!in) return false;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("kind", "") != "project") return false;
    out = Project{};
    out.path = dir;
    out.tmpl = j.value("template", "");
    out.id = j.value("id", "");
    out.title = j.value("title", "");
    out.created = j.value("created", 0ll);
    out.rules = rules_from(j);
    return true;
}

std::vector<Project> projects(const std::string& home, const std::vector<Template>& ts) {
    std::vector<Project> out;
    std::set<std::string> roots;
    for (const auto& t : ts) if (t.problem.empty()) roots.insert(home + "/" + t.root);
    for (const auto& r : roots) {
        if (!is_dir(r)) continue;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(r, ec)) {
            if (!is_dir(e.path().string())) continue;
            Project p;
            if (load_project(e.path().string(), p)) out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const Project& a, const Project& b) { return a.created > b.created; });
    return out;
}

// ── naming ──────────────────────────────────────────────────────────────────
bool fits(const Project& p, const std::string& filename, Parts* out) {
    const std::string& pat = p.rules.pattern;
    if (pat.empty()) return true;
    std::string re;
    int group = 0, g_part = 0, g_ext = 0, g_stamp = 0;
    for (std::size_t i = 0; i < pat.size();) {
        if (pat[i] == '{') {
            const auto e = pat.find('}', i);
            if (e != std::string::npos) {
                const std::string tk = pat.substr(i, e - i + 1);
                bool known = true;
                if (tk == "{id}") re += regex_escape(slug(p.id));
                else if (tk == "{title}") re += regex_escape(slug(p.title));
                else if (tk == "{part}") { re += "(.+?)"; g_part = ++group; }
                else if (tk == "{ext}") { re += "([A-Za-z0-9]+)"; g_ext = ++group; }
                else if (tk == "{yyyymmdd-hhmm}") { re += "([0-9]{8}-[0-9]{4})"; g_stamp = ++group; }
                else if (tk == "{yyyymmdd}") { re += "([0-9]{8})"; g_stamp = ++group; }
                else if (tk == "{date}") re += "[0-9]{4}-[0-9]{2}-[0-9]{2}";
                else if (tk == "{year}") re += "[0-9]{4}";
                else known = false;
                if (known) { i = e + 1; continue; }
            }
        }
        re += regex_escape(std::string(1, pat[i]));
        ++i;
    }
    std::smatch m;
    try {
        if (!std::regex_match(filename, m, std::regex(re))) return false;
    } catch (const std::regex_error&) { return false; }
    if (out) {
        out->part = g_part ? m[g_part].str() : "";
        out->ext = g_ext ? m[g_ext].str() : "";
        out->stamp = g_stamp ? m[g_stamp].str() : "";
    }
    return true;
}

std::string name_for(const Project& p, const std::string& part, const std::string& ext, std::int64_t when) {
    std::string s = p.rules.pattern;
    if (ext.empty()) replace_all(s, ".{ext}", "");
    std::string pt = slug(part);
    if (pt.empty()) pt = "file";
    replace_all(s, "{part}", pt);
    replace_all(s, "{ext}", lower(ext));
    return fill(s, Values{p.id, p.title, when});
}

std::string route(const Project& p, const std::string& ext) {
    const std::string e = lower(ext);
    for (const auto& r : p.rules.routes)
        if (std::find(r.exts.begin(), r.exts.end(), e) != r.exts.end()) return r.folder;
    return {};
}

std::vector<Entry> contents(const Project& p, std::size_t limit) {
    std::vector<Entry> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p.path, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator() && out.size() < limit; it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        struct stat st{};
        if (::lstat(it->path().c_str(), &st) != 0) continue;
        if (!name.empty() && name[0] == '.') { if (S_ISDIR(st.st_mode)) it.disable_recursion_pending(); continue; }
        if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) continue;
        Entry e;
        e.rel = fs::relative(it->path(), p.path, ec).string();
        e.fits = fits(p, name);
        e.versioned = e.rel.rfind(p.rules.versions + "/", 0) == 0;
        e.bytes = static_cast<std::uint64_t>(st.st_size);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.rel < b.rel; });
    return out;
}

// ── file into project ───────────────────────────────────────────────────────
std::vector<Incoming> propose(const std::string& home, const Project& p, const std::vector<std::string>& paths) {
    std::vector<Incoming> out;
    std::set<std::string> taken;
    for (const auto& raw : paths) {
        Incoming in;
        in.from = fs::path(raw).lexically_normal().string();
        struct stat st{};
        if (::lstat(in.from.c_str(), &st) != 0) { in.note = "not there"; in.checked = false; out.push_back(in); continue; }
        in.bytes = static_cast<std::uint64_t>(st.st_size);
        in.dev = st.st_dev;
        in.ino = st.st_ino;
        in.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        const auto no = [&](std::string why) { in.note = std::move(why); in.checked = false; out.push_back(in); };
        if (S_ISDIR(st.st_mode)) {
            no(pkg::is_package_name(fs::path(in.from).filename().string())
                   ? "a package -- kept whole; only files are filed"
                   : "a folder -- only files are filed");
            continue;
        }
        if (!S_ISREG(st.st_mode)) { no("not a plain file"); continue; }
        if (in.from.rfind(home + "/", 0) != 0) { no("outside your home folder -- Rouster moves only inside it"); continue; }
        const std::string name = fs::path(in.from).filename().string();
        if (!name.empty() && name[0] == '.') { no("a hidden file"); continue; }
        if (const auto pk = pkg::enclosing(in.from.substr(home.size() + 1)); !pk.empty()) {   // s015a
            no("inside a package (~/" + pk + ") -- kept whole, never split");
            continue;
        }
        const std::string ext = ext_of(name);
        const std::string folder = route(p, ext);
        const std::string dir = folder.empty() ? p.path : p.path + "/" + folder;
        const bool keep = p.rules.pattern.empty() || fits(p, name);
        const std::int64_t when = static_cast<std::int64_t>(st.st_mtim.tv_sec);
        const auto make = [&](int n) {
            if (keep) {
                if (n == 1) return name;
                const std::string e = ext_of(name), s = stem_of(name);
                return s + "-" + std::to_string(n) + (e.empty() ? "" : name.substr(s.size()));
            }
            return name_for(p, n == 1 ? stem_of(name) : stem_of(name) + "-" + std::to_string(n), ext, when);
        };
        std::string to;
        for (int n = 1; n < 1000; ++n) {
            to = dir + "/" + make(n);
            if (to == in.from) break;
            if (!exists(to) && !taken.count(to)) break;
        }
        if (to == in.from) { no("already filed"); continue; }
        taken.insert(to);
        in.to = to;
        out.push_back(in);
    }
    return out;
}

door::Plan file_plan(const Project& p, const std::vector<Incoming>& items, std::vector<std::string>& dropped) {
    door::Plan plan;
    std::vector<door::Action> moves;
    std::vector<std::string> made;
    for (const auto& in : items) {
        if (!in.checked || in.to.empty()) continue;
        struct stat st{};
        if (::lstat(in.from.c_str(), &st) != 0) { dropped.push_back(in.from + " -- gone since"); continue; }
        const std::int64_t m = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
        if (st.st_dev != in.dev || st.st_ino != in.ino || m != in.mtime_ns) {
            dropped.push_back(in.from + " -- changed since");
            continue;
        }
        if (exists(in.to)) { dropped.push_back(in.to + " -- that name is taken now"); continue; }
        make_path(p.path, fs::path(in.to).parent_path().string(), made, plan.actions, "a folder the project routes to");
        door::Action a;
        a.verb = door::Verb::Move;
        a.from = in.from;
        a.to = in.to;
        a.bytes = in.bytes;
        a.reason = "named by the project's pattern, routed by type";
        moves.push_back(a);
    }
    for (auto& a : moves) plan.actions.push_back(std::move(a));
    const std::size_t n = moves.size();
    plan.label = "File into " + fs::path(p.path).filename().string() + ": " + std::to_string(n) + (n == 1 ? " file" : " files");
    return plan;
}

// ── new version ─────────────────────────────────────────────────────────────
door::Plan new_version(const Project& p, const std::string& rel, std::int64_t now, std::string& why) {
    door::Plan plan;
    const std::string pat = p.rules.pattern;
    if (pat.find("{yyyymmdd-hhmm}") == std::string::npos && pat.find("{yyyymmdd}") == std::string::npos) {
        why = "the project's pattern has no date to stamp";
        return plan;
    }
    const std::string abs = p.path + "/" + rel;
    struct stat st{};
    if (::lstat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { why = "not a file: " + abs; return plan; }
    if (rel.rfind(p.rules.versions + "/", 0) == 0) { why = "already an old version"; return plan; }
    const std::string name = fs::path(abs).filename().string();
    Parts parts;
    if (!fits(p, name, &parts)) { why = "its name doesn't fit the pattern"; return plan; }
    const std::string fresh = name_for(p, parts.part, parts.ext, now);
    if (fresh == name) { why = "a version was made this minute -- wait a minute"; return plan; }
    const std::string to = fs::path(abs).parent_path().string() + "/" + fresh;
    if (exists(to)) { why = "already there: " + fresh; return plan; }
    const std::string vdir = p.path + "/" + p.rules.versions;
    const std::string old = vdir + "/" + name;
    if (exists(old)) { why = "already in " + p.rules.versions + "/: " + name; return plan; }
    if (!exists(vdir)) plan.actions.push_back(make_dir(vdir, "where old versions go"));
    door::Action c;
    c.verb = door::Verb::Copy;
    c.from = abs;
    c.to = to;
    c.bytes = static_cast<std::uint64_t>(st.st_size);
    c.reason = "the new version, stamped now";
    plan.actions.push_back(c);
    door::Action m;
    m.verb = door::Verb::Move;
    m.from = abs;
    m.to = old;
    m.bytes = c.bytes;
    m.reason = "the old version, out of the way";
    plan.actions.push_back(m);
    plan.label = "New version: " + fresh;
    return plan;
}

}  // namespace rouster::core::organize
