#pragma once
#include "core/Door.hpp"

#include <cstdint>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Organize (s010, REQUIREMENTS §4.7) -- templates, naming, file into project,
// new version. GTK-free, READ-ONLY: every function looks, and returns a plan;
// the door acts.
//
// A TEMPLATE is a plain folder under ~/.config/rouster/templates/<Name>/,
// edited in Files like any folder. Names in it may carry placeholders:
//   {id} {title} {date} (yyyy-mm-dd) {year} {yyyymmdd} {yyyymmdd-hhmm}
// A small text file (<= 64 KB, no NUL bytes) with a placeholder INSIDE it is
// written with them filled in; any other file is copied as it is (Copy verb).
// Its `.rouster.json` says the rest:
//   { "kind": "template",
//     "root":    "Documents/Jobs",        where its projects go, under home
//     "folder":  "{id}_{title}",          the project folder's name
//     "pattern": "{id}_{title}_{yyyymmdd-hhmm}_{part}.{ext}",   "" = no renaming
//     "versions": "versions",             where New version puts the old one
//     "routes": { "fonts": ["ttf","otf"], "images": ["jpg","png"] } }
//
// A PROJECT is a folder holding a `.rouster.json` of kind "project" (written
// when it is made): its template, id, title, pattern, routes. Every project
// carries its own rules, so editing a template later changes no project.
//
// Values in names are made safe: spaces and _ / \ : * ? " < > | become '-'
// ("_" is the pattern's separator, so a title never contains one).
// ─────────────────────────────────────────────────────────────────────────────
namespace rouster::core::organize {

inline constexpr const char* kTemplates = ".config/rouster/templates";   // under home
inline constexpr const char* kMeta = ".rouster.json";

struct Route {
    std::string folder;
    std::vector<std::string> exts;   // lower case, no dot
};

struct Rules {                        // what a template hands each project
    std::string pattern;
    std::string versions = "versions";
    std::vector<Route> routes;
};

struct Template {
    std::string name;                // the folder's name
    std::string path;                // absolute
    std::string root = "Documents";  // under home
    std::string folder = "{id}_{title}";
    Rules       rules;
    std::string problem;             // non-empty: its .rouster.json could not be read
};

struct Project {
    std::string path;                // absolute
    std::string tmpl, id, title;
    std::int64_t created = 0;
    Rules       rules;
};

struct Values {
    std::string  id, title;          // as typed
    std::int64_t when = 0;           // unix seconds: {date} {year} {yyyymmdd...}
};

std::string slug(const std::string& s);
// Fill the placeholders. `names` true: values slugged (for paths); false: as typed (for text).
std::string fill(const std::string& s, const Values& v, bool names = true);
std::string stamp(std::int64_t when, bool minutes = true);   // 20261106-1520 / 20261106 (local time)

// The templates there are now, by name. READ-ONLY.
std::vector<Template> templates(const std::string& home);

// The starter templates (gtkmm app, Design job, Book) as a plan: MakeDir +
// Write. One already there (by name) is left alone and named in `skipped`.
door::Plan starters(const std::string& home, std::vector<std::string>& skipped);

// A new project from a template: its folder, the template's tree, its
// .rouster.json. Empty plan + `problems` when it can't be made.
door::Plan new_project(const std::string& home, const Template& t, const Values& v, std::vector<std::string>& problems);
std::string project_dir(const std::string& home, const Template& t, const Values& v);   // where it would go

bool load_project(const std::string& dir, Project& out);
// The projects in each template's root (one level down). READ-ONLY.
std::vector<Project> projects(const std::string& home, const std::vector<Template>& ts);

// Naming.
struct Parts { std::string part, ext, stamp; };
bool fits(const Project& p, const std::string& filename, Parts* out = nullptr);
std::string name_for(const Project& p, const std::string& part, const std::string& ext, std::int64_t when);
std::string route(const Project& p, const std::string& ext);   // "" = the project's top

// What is in the project now: every file (hidden entries and .rouster.json
// left out), whether its name fits, whether it is in the versions folder.
struct Entry { std::string rel; bool fits = false, versioned = false; std::uint64_t bytes = 0; };
std::vector<Entry> contents(const Project& p, std::size_t limit = 2000);

// File into project: for each path, where it would go (READ-ONLY).
struct Incoming {
    std::string   from;              // absolute
    std::string   to;                // absolute; "" = it can't be filed (note says why)
    std::string   note;
    std::uint64_t bytes = 0, dev = 0, ino = 0;
    std::int64_t  mtime_ns = 0;
    bool          checked = true;
};
std::vector<Incoming> propose(const std::string& home, const Project& p, const std::vector<std::string>& paths);
// The checked ones: MakeDir for missing route folders, then one Move each.
// Each is looked at again; changed, gone, or its name taken now -> left out.
door::Plan file_plan(const Project& p, const std::vector<Incoming>& items, std::vector<std::string>& dropped);

// New version of `rel` (a file in the project whose name fits): a copy under
// the name stamped `now`, then the old one into the versions folder.
door::Plan new_version(const Project& p, const std::string& rel, std::int64_t now, std::string& why);

}  // namespace rouster::core::organize
