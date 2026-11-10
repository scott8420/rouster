// Shell_versions.cpp -- s017: version families on the Tidy page.
//
// The card: one block per family -- the keeper first (KEEP · why), then each
// older save with its date, size and, if it looks like one, why it's a reflex
// save; "Keep this" on any of them makes it the keeper (on screen until the
// step runs, then remembered). Checked families go in ONE door step: the
// versions folder made if missing, the older saves moved into it. Nothing is
// pre-checked -- "which one is the good one" is yours to answer.
// App-managed history (Folio, jot, Trails) is listed and left alone.
#include "Shell.hpp"
#include "Log.hpp"
#include "core/Scan.hpp"
#include "core/Tidy.hpp"
#include "core/Compare.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/grid.h>
#include <gtkmm/picture.h>
#include <gdkmm/texture.h>
#include <giomm/appinfo.h>
#include <giomm/file.h>
#include <glibmm/convert.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace rouster {
namespace vs = core::versions;

namespace {

constexpr std::size_t kFamiliesShown = 60;

Gtk::Label* vtext(const std::string& s, const char* css, float x = 0.0f) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    if (css) l->add_css_class(css);
    l->set_xalign(x);
    return l;
}
std::string commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}
std::string name_of(const std::string& rel) {
    const auto k = rel.rfind('/');
    return k == std::string::npos ? rel : rel.substr(k + 1);
}
std::string ago(std::int64_t t, std::int64_t now) {
    if (t <= 0) return "";
    const std::int64_t d = now - t;
    if (d < 3600) return "just now";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    return core::tidy::age(d) + " ago";
}
std::string plural(std::size_t n, const char* one, const char* many) { return commas(n) + " " + (n == 1 ? one : many); }
std::string when(std::int64_t t) {   // "2026-11-08 14:32"
    if (t <= 0) return "";
    const auto tt = static_cast<std::time_t>(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string Shell::versions_data_home() const {  // helper: s017 -- where the apps keep their data (a fixture's own)
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h) return std::string(h) + "/.local/share";
    return Glib::get_user_data_dir();
}
std::string Shell::versions_config_home() const {  // helper: s017 -- where the apps keep their settings
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h) return std::string(h) + "/.config";
    return Glib::get_user_config_dir();
}

std::size_t Shell::versions_keeper(const vs::Family& f) const {  // helper: s017 -- the keeper on screen (yours, else found)
    if (auto it = m_vf_keep.find(f.key()); it != m_vf_keep.end())
        for (std::size_t x = 0; x < f.members.size(); ++x)
            if (f.members[x].rel == it->second) return x;
    return f.keeper;
}

void Shell::paint_versions() {  // helper: s017 -- the Version families card + App history, into the Tidy list
    const auto& r = m_vf_shown;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    m_vf_go = nullptr;
    m_vf_gate = nullptr;
    m_vf_painted.clear();
    if (!r.ok) {
        if (!r.error.empty()) {
            auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            c->add_css_class("card");
            c->append(*vtext("Version families", "card-title"));
            c->append(*vtext("Not looked: " + r.error, "pf-amber"));
            m_td_list.append(*c);
        }
        return;
    }
    if (!r.families.empty()) {
        std::size_t tidy = 0;
        for (const auto& f : r.families) if (!f.loose) ++tidy;
        auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        c->add_css_class("card");
        c->append(*vtext("Version families · " + commas(r.families.size()) +
                         (r.loose ? " · " + plural(r.loose, "older save", "older saves") + " loose · " +
                                        core::scan::human_bytes(r.loose_bytes)
                                  : std::string(" · all tidy")), "card-title"));
        auto sub = vtext("Save, save, save -- the same name with a mark on the end (final, v2, (1), Copy of). The newest is "
                         "the keeper and stays; check a family to put its older saves in “<name> versions” beside it. "
                         "Nothing is checked for you. “Keep this” makes another one the keeper." +
                         (tidy ? " " + plural(tidy, "family is", "families are") + " already tidy." : std::string()),
                         "card-sub");
        sub->set_wrap(true);
        c->append(*sub);
        if (!m_vf_said.empty()) {
            auto said = vtext(m_vf_said, "pf-green");
            said->set_wrap(true);
            c->append(*said);
        }
        auto list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
        std::size_t shown = 0;
        for (const auto& f : r.families) {
            if (shown++ >= kFamiliesShown) break;
            const std::string key = f.key();
            const std::size_t k = versions_keeper(f);
            const bool moved_keeper = k != f.keeper;
            std::size_t loose = 0, reflex = 0;
            std::uint64_t lb = 0;
            for (std::size_t x = 0; x < f.members.size(); ++x) {
                if (x == k) continue;
                if (!f.members[x].reflex.empty()) ++reflex;
                if (!f.members[x].in_versions) { ++loose; lb += f.members[x].bytes; }
            }
            const bool actionable = loose > 0 || f.members[k].in_versions;
            auto fam = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
            auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            if (actionable) {
                m_vf_painted.insert(key);
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                cb->set_active(m_vf_checked.count(key) > 0);
                cb->set_valign(Gtk::Align::CENTER);
                cb->signal_toggled().connect([this, cb, key] {
                    if (cb->get_active()) m_vf_checked.insert(key); else m_vf_checked.erase(key);
                    versions_gate();
                });
                head->append(*cb);
            }
            const std::string where = f.folder.empty() ? "~/" : "~/" + f.folder + "/";
            auto title = vtext(where + f.base + (f.ext.empty() ? "" : "." + f.ext), "detail-path");
            title->set_hexpand(true);
            title->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            title->set_tooltip_text("Older saves go to " + m_home + "/" + f.vdir);
            head->append(*title);
            std::string summary = plural(f.members.size(), "save", "saves") + " · " +
                                  (actionable ? (loose ? commas(loose) + " loose" : std::string("keeper comes out")) : "tidy");
            if (reflex) summary += " · " + commas(reflex) + " reflex";
            head->append(*vtext(summary, "dim", 1.0f));
            if (actionable) {
                auto sz = vtext(core::scan::human_bytes(lb), "detail-size", 1.0f);
                sz->set_width_chars(9);
                head->append(*sz);
            }
            auto cmp = Gtk::make_managed<Gtk::Button>("Compare");
            cmp->add_css_class("ghost");
            cmp->add_css_class("small");
            cmp->set_tooltip_text("Side by side: the keeper on the left, each older save on the right.");
            cmp->signal_clicked().connect([this, key] {
                Glib::signal_idle().connect_once([this, key] { open_versions_viewer(key); });
            });
            head->append(*cmp);
            auto no = Gtk::make_managed<Gtk::Button>("Not a family");
            no->add_css_class("ghost");
            no->add_css_class("small");
            no->set_tooltip_text("These aren't versions of one thing -- never proposed again.");
            no->signal_clicked().connect([this, key] {
                Glib::signal_idle().connect_once([this, key] { on_versions_not_family(key); });
            });
            head->append(*no);
            fam->append(*head);

            for (std::size_t x = 0; x < f.members.size(); ++x) {
                const auto& m = f.members[x];
                auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
                row->set_margin_start(34);
                auto n = vtext((m.in_versions ? "↳ " : "") + name_of(m.rel) + (m.package ? "  (whole)" : ""),
                               x == k ? "detail-path" : "dim");
                n->set_hexpand(true);
                n->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
                n->set_tooltip_text(m_home + "/" + m.rel);
                row->append(*n);
                row->append(*vtext(ago(m.changed, now), "dim", 1.0f));
                auto sz = vtext(core::scan::human_bytes(m.bytes), "detail-size", 1.0f);
                sz->set_width_chars(9);
                row->append(*sz);
                std::string chip = x == k ? (moved_keeper ? std::string("KEEP · you chose")
                                            : f.keeper_why == "the only one outside its versions folder" ? std::string("KEEP")
                                                                                                      : "KEEP · " + f.keeper_why)
                                 : m.in_versions ? "in versions"
                                 : !m.reflex.empty() ? "reflex" : "";
                if (!chip.empty()) {
                    auto ch = vtext(chip, "td-query", 0.5f);
                    ch->set_valign(Gtk::Align::CENTER);
                    if (!m.reflex.empty() && x != k) ch->set_tooltip_text("Looks like a reflex save: " + m.reflex);
                    row->append(*ch);
                }
                if (x != k) {
                    auto keep = Gtk::make_managed<Gtk::Button>("Keep this");
                    keep->add_css_class("ghost");
                    keep->add_css_class("small");
                    keep->set_tooltip_text("Make this one the keeper: it stays (or comes out of the versions folder); "
                                           "the others go in.");
                    const std::string rel = m.rel;
                    keep->signal_clicked().connect([this, key, rel] {
                        Glib::signal_idle().connect_once([this, key, rel] {
                            m_vf_keep[key] = rel;
                            m_vf_checked.insert(key);
                            if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: keep this '{}' (on screen)", rel);
                            m_vf_said = name_of(rel) + " is the keeper of its family now -- checked; press the button to sort them.";
                            versions_repaint();
                        });
                    });
                    row->append(*keep);
                }
                fam->append(*row);
            }
            list->append(*fam);
        }
        c->append(*list);
        if (r.families.size() > kFamiliesShown)
            c->append(*vtext("... and " + commas(r.families.size() - kFamiliesShown) + " more -- not shown, never checked", "dim"));
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        bar->set_margin_top(6);
        auto gate = vtext("", "dim");
        gate->set_hexpand(true);
        gate->set_wrap(true);
        bar->append(*gate);
        auto go = Gtk::make_managed<Gtk::Button>("Sort versions");
        go->add_css_class("act");
        go->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_versions_act));
        bar->append(*go);
        c->append(*bar);
        m_vf_go = go;
        m_vf_gate = gate;
        m_td_list.append(*c);
    }

    if (!r.history.empty()) {
        auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        c->add_css_class("card");
        c->append(*vtext("Kept by our apps · " + commas(r.history.size()), "card-title"));
        auto sub = vtext("History an app keeps for itself (it prunes on its own schedule), and Samson's folder. Recognised "
                         "and left alone; never a family. Samson's folder and its containers are never touched at all.",
                         "card-sub");
        sub->set_wrap(true);
        c->append(*sub);
        for (const auto& h : r.history) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->set_margin_start(6);
            const std::string what = h.app == "Trails" ? "Trails' recordings"
                                   : h.app == "Samson" ? "Samson's folder, ~/" + h.path.substr(m_home.size() + 1)
                                   : h.app + "'s history of " + h.what;
            auto l = vtext(what, "detail-path");
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(h.path);
            row->append(*l);
            const char* unit = h.app == "jot" ? " days" : h.app == "Trails" ? " recordings" : h.app == "Samson" ? " sealed" : " versions";
            row->append(*vtext(commas(h.count) + unit + (h.newest ? ", newest " + ago(h.newest, now) : ""), "dim", 1.0f));
            auto sz = vtext(core::scan::human_bytes(h.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            row->append(*sz);
            auto ch = vtext(h.app == "Samson" ? std::string("never touched") : "managed by " + h.app, "td-query", 0.5f);
            ch->set_valign(Gtk::Align::CENTER);
            row->append(*ch);
            c->append(*row);
        }
        m_td_list.append(*c);
    }
    versions_gate();
}

void Shell::versions_gate() {  // helper: s017 -- the families card's button: label + may it act
    if (!m_vf_go || !m_vf_gate) return;
    const bool busy = m_td_worker.joinable() || door_busy();
    std::size_t fams = 0, saves = 0;
    std::uint64_t bytes = 0;
    for (const auto& f : m_vf_shown.families) {
        const std::string key = f.key();
        if (!m_vf_checked.count(key) || !m_vf_painted.count(key)) continue;
        ++fams;
        const std::size_t k = versions_keeper(f);
        for (std::size_t x = 0; x < f.members.size(); ++x)
            if (x != k && !f.members[x].in_versions) { ++saves; bytes += f.members[x].bytes; }
    }
    m_vf_go->set_label(saves ? "Put " + plural(saves, "older save", "older saves") + " beside " +
                                   (fams == 1 ? std::string("its keeper") : "their keepers")
                             : fams ? std::string("Swap the keeper") : std::string("Sort versions"));
    m_vf_go->set_sensitive(fams > 0 && !m_pf_red && !busy);
    m_vf_gate->set_text(m_pf_red ? "Preflight has a red check. Tidy waits until it clears -- see Preflight."
                        : !fams ? "Nothing checked."
                                : plural(fams, "family", "families") + " checked, " + core::scan::human_bytes(bytes) +
                                      ". One step; Undo takes it back. Hover a name to see its versions folder.");
}

void Shell::on_versions_act() {  // handler: s017 -- the checked families -> one door step
    if (!m_door || door_busy() || m_td_worker.joinable()) return;
    if (m_td_states.get_visible_child_name() != "results") return;   // preview first: only from the card
    paint_preflight();
    if (m_pf_red) { versions_gate(); return; }
    std::vector<vs::Family> fams;
    std::vector<std::size_t> keepers;
    for (const auto& f : m_vf_shown.families)
        if (m_vf_checked.count(f.key()) && m_vf_painted.count(f.key())) {
            fams.push_back(f);
            keepers.push_back(versions_keeper(f));
        }
    if (fams.empty()) return;
    auto step = vs::plan_all(m_home, fams, keepers);
    if (auto lg = log::get(log::Area::Tidy)) {
        lg->info("versions: '{}': {} families, {} moves, {} actions", step.plan.label, fams.size(), step.moves,
                 step.plan.actions.size());
        for (const auto& d : step.dropped) lg->info("  left out: {}", d);
    }
    if (step.plan.actions.empty()) {
        if (m_vf_gate)
            m_vf_gate->set_text(step.dropped.empty() ? std::string("Nothing to do.") : step.dropped.front() + " -- Look again.");
        return;
    }
    step.plan.previewed = true;   // the card on screen was the preview
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const auto r = m_door->run(step.plan);
    std::size_t chosen = 0;
    if (r.ok)
        for (std::size_t x = 0; x < fams.size(); ++x)
            if (keepers[x] != fams[x].keeper && vs::choose_keeper(data_dir(), fams[x], keepers[x], now)) ++chosen;
    if (auto lg = log::get(log::Area::Tidy)) {
        lg->info("versions: {} done, {} failed; {} keepers remembered", r.done, r.failed, chosen);
        for (const auto& m : r.messages) lg->warn("  {}", m);
    }
    m_vf_keep.clear();
    m_vf_checked.clear();
    const std::string left = step.dropped.empty() ? "" : " " + std::to_string(step.dropped.size()) +
                             " left out (changed since the Look, or a name taken) -- see the log.";
    if (r.ok) {
        tidy_done(plural(step.moves, "older save", "older saves"),
                  "in their versions folders now, beside " + plural(fams.size(), "keeper", "keepers") +
                  ". Undo takes the step back." + left);
    } else if (r.done == 0) {
        tidy_done("Nothing moved", r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        tidy_done(std::to_string(r.done) + " of " + std::to_string(step.plan.actions.size()),
                  "steps done; " + std::to_string(r.failed) + " could not: " +
                  (r.messages.empty() ? std::string("see Activity") : r.messages.front()) + ". Undo puts back what moved.");
    }
}

void Shell::on_versions_not_family(const std::string& key) {  // handler: s017 -- "Not a family": remembered, never proposed again
    for (const auto& f : m_vf_shown.families) {
        if (f.key() != key) continue;
        const bool ok = vs::not_a_family(data_dir(), f, static_cast<std::int64_t>(std::time(nullptr)));
        if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: not a family '{}': {}", key, ok ? "remembered" : "FAILED");
        m_vf_checked.erase(key);
        m_vf_keep.erase(key);
        const std::string said = ok ? f.base + " -- not a family; never proposed again." : "Couldn't remember that -- see the log.";
        versions_repaint([this, said] {
            tidy_rereport("");
            m_vf_said = said;
        });
        return;
    }
}

void Shell::versions_repaint(const std::function<void()>& change) {  // helper: s017 -- repaint the Tidy list, scroll kept
    auto* sw = dynamic_cast<Gtk::ScrolledWindow*>(m_td_list.get_ancestor(Gtk::ScrolledWindow::get_type()));
    const double at = sw ? sw->get_vadjustment()->get_value() : 0.0;
    if (change) change();
    paint_tidy();
    if (sw)
        Glib::signal_idle().connect_once([sw, at] { sw->get_vadjustment()->set_value(at); });
}

// ── s017b: the viewer ────────────────────────────────────────────────────────
// TIDY "Version families": left = the keeper, fixed; right = one version with
// ◀ ▶ ("3 of 7", date, size), and under it Keep this one instead / Short life.
// Pictures both at ONE scale (so a smaller save looks smaller); texts both,
// changed lines marked (core::compare's line diff), only the changes unless
// you ask for the whole; our apps' files: date, size, Open in Curvz.
// Reading for the viewer is read-only; Short life is one door step.
namespace {

constexpr int         kPaneW = 440, kPaneH = 360;   // the box each picture is fitted in -- the same for both
constexpr std::size_t kRowsShown = 1500;            // text rows painted; past that, said
constexpr std::uint64_t kPictureCap = 64ull << 20;  // a bigger picture isn't loaded here

const vs::Family* family_of(const vs::Report& r, const std::string& key) {
    for (const auto& f : r.families)
        if (f.key() == key) return &f;
    return nullptr;
}

Gtk::Label* row_label(const std::string& s, const char* css) {
    auto l = Gtk::make_managed<Gtk::Label>(s.empty() ? " " : s);
    l->add_css_class("vv-line");
    if (css) l->add_css_class(css);
    l->set_xalign(0.0f);
    l->set_yalign(0.0f);
    l->set_wrap(true);
    l->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    l->set_width_chars(16);
    l->set_max_width_chars(72);
    l->set_hexpand(true);
    return l;
}

}  // namespace

void Shell::open_versions_viewer(const std::string& key) {  // handler: s017b -- a family's Compare -> the viewer
    const auto* f = family_of(m_vf_shown, key);
    if (!f) return;
    m_vv_key = key;
    m_vv_all = false;
    m_vv_said.clear();
    m_vv_rel.clear();
    const std::size_t k = versions_keeper(*f);
    for (std::size_t x = 0; x < f->members.size(); ++x)
        if (x != k) { m_vv_rel = f->members[x].rel; break; }
    if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: viewer '{}' ({} saves)", key, f->members.size());
    paint_versions_viewer();
    m_td_states.set_visible_child("viewer");
}

void Shell::paint_versions_viewer() {  // helper: s017b -- keeper left, one version right, ◀ ▶
    namespace cm = core::compare;
    while (auto* ch = m_vv_box.get_first_child()) m_vv_box.remove(*ch);
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
    auto back = Gtk::make_managed<Gtk::Button>("‹ Back");
    back->add_css_class("ghost");
    back->set_valign(Gtk::Align::CENTER);
    back->signal_clicked().connect([this] {
        Glib::signal_idle().connect_once([this] {
            m_vv_key.clear();
            m_td_states.set_visible_child("results");
            versions_repaint();
        });
    });
    head->append(*back);
    const auto* f = family_of(m_vf_shown, m_vv_key);
    auto title = vtext(f ? (f->folder.empty() ? "~/" : "~/" + f->folder + "/") + f->base + (f->ext.empty() ? "" : "." + f->ext)
                         : std::string("Version families"), "hero-title", 0.0f);
    title->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    title->set_hexpand(true);
    head->append(*title);
    if (f) head->append(*vtext(plural(f->members.size(), "save", "saves"), "dim", 1.0f));
    m_vv_box.append(*head);
    if (!m_vv_said.empty()) {
        auto said = vtext(m_vv_said, "pf-green");
        said->set_wrap(true);
        m_vv_box.append(*said);
    }
    const std::size_t k = f ? versions_keeper(*f) : 0;
    std::vector<std::size_t> others;
    if (f)
        for (std::size_t x = 0; x < f->members.size(); ++x)
            if (x != k) others.push_back(x);
    if (!f || others.empty()) {
        m_vv_box.append(*vtext(f ? "Only the keeper is left -- nothing to set beside it." : "Nothing left to compare in this family.", "dim"));
        return;
    }
    std::size_t pos = 0;
    for (std::size_t x = 0; x < others.size(); ++x)
        if (f->members[others[x]].rel == m_vv_rel) pos = x;
    const vs::Member& L = f->members[k];
    const vs::Member& R = f->members[others[pos]];
    m_vv_rel = R.rel;
    const std::string la = m_home + "/" + L.rel, ra = m_home + "/" + R.rel;
    const std::string app = cm::app_of(f->ext);
    const std::string open_label = app.empty() ? std::string("Open") : "Open in " + app;

    // ── the two heads ──
    auto cols = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 24);
    cols->set_homogeneous(true);
    const auto meta = [&](const vs::Member& m) {
        return when(m.changed) + " · " + ago(m.changed, now) + " · " + core::scan::human_bytes(m.bytes);
    };
    const auto opener = [&](const std::string& rel) {
        auto b = Gtk::make_managed<Gtk::Button>(open_label);
        b->add_css_class("ghost");
        b->add_css_class("small");
        b->set_tooltip_text(app.empty() ? "Open it in the app that opens this kind of file. Rouster changes nothing."
                                        : "Open it in " + app + ". Rouster changes nothing.");
        b->signal_clicked().connect([this, rel, app] { versions_open_file(rel, app); });
        return b;
    };
    {
        auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        c->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        const bool chose = k != f->keeper;
        auto chip = vtext(chose ? std::string("KEEP · you chose")
                          : f->keeper_why == "the only one outside its versions folder" ? std::string("KEEP")
                                                                                     : "KEEP · " + f->keeper_why,
                          "td-query", 0.5f);
        top->append(*chip);
        auto sp = Gtk::make_managed<Gtk::Box>();
        sp->set_hexpand(true);
        top->append(*sp);
        top->append(*opener(L.rel));
        c->append(*top);
        auto n = vtext(name_of(L.rel) + (L.package ? "  (whole)" : ""), "detail-path");
        n->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        n->set_tooltip_text(la);
        c->append(*n);
        c->append(*vtext(meta(L), "dim"));
        cols->append(*c);
    }
    {
        auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        c->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto prev = Gtk::make_managed<Gtk::Button>("◀");
        prev->add_css_class("ghost");
        prev->add_css_class("small");
        prev->set_tooltip_text("Newer");
        prev->set_sensitive(pos > 0);
        auto next = Gtk::make_managed<Gtk::Button>("▶");
        next->add_css_class("ghost");
        next->add_css_class("small");
        next->set_tooltip_text("Older");
        next->set_sensitive(pos + 1 < others.size());
        const std::string prel = pos > 0 ? f->members[others[pos - 1]].rel : "";
        const std::string nrel = pos + 1 < others.size() ? f->members[others[pos + 1]].rel : "";
        prev->signal_clicked().connect([this, prel] {
            Glib::signal_idle().connect_once([this, prel] { m_vv_rel = prel; m_vv_said.clear(); paint_versions_viewer(); });
        });
        next->signal_clicked().connect([this, nrel] {
            Glib::signal_idle().connect_once([this, nrel] { m_vv_rel = nrel; m_vv_said.clear(); paint_versions_viewer(); });
        });
        top->append(*prev);
        top->append(*vtext(commas(pos + 1) + " of " + commas(others.size()), "detail-size", 0.5f));
        top->append(*next);
        const std::string chip = R.in_versions ? "in versions" : !R.reflex.empty() ? "reflex" : "";
        if (!chip.empty()) {
            auto ch = vtext(chip, "td-query", 0.5f);
            if (!R.reflex.empty()) ch->set_tooltip_text("Looks like a reflex save: " + R.reflex);
            top->append(*ch);
        }
        auto sp = Gtk::make_managed<Gtk::Box>();
        sp->set_hexpand(true);
        top->append(*sp);
        top->append(*opener(R.rel));
        c->append(*top);
        auto n = vtext((R.in_versions ? "↳ " : "") + name_of(R.rel) + (R.package ? "  (whole)" : ""), "detail-path");
        n->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        n->set_tooltip_text(ra);
        c->append(*n);
        c->append(*vtext(meta(R), "dim"));
        auto acts = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        acts->set_margin_top(6);
        auto keep = Gtk::make_managed<Gtk::Button>("Keep this one instead");
        keep->add_css_class("ghost");
        keep->add_css_class("small");
        keep->set_tooltip_text("Make this one the keeper (on screen until the card's button sorts the family). "
                               "The old keeper moves over here.");
        const std::string key = m_vv_key, rrel = R.rel, lrel = L.rel;
        keep->signal_clicked().connect([this, key, rrel, lrel] {
            Glib::signal_idle().connect_once([this, key, rrel, lrel] {
                m_vf_keep[key] = rrel;
                m_vf_checked.insert(key);
                m_vv_rel = lrel;   // the old keeper, now on the right
                m_vv_said = name_of(rrel) + " is the keeper now -- the family is checked. Back, then the card's button "
                            "puts the others in its versions folder.";
                m_vf_said = name_of(rrel) + " is the keeper of its family now -- checked; press the button to sort them.";
                if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: viewer: keep this one instead '{}' (on screen)", rrel);
                paint_versions_viewer();
            });
        });
        acts->append(*keep);
        auto shortl = Gtk::make_managed<Gtk::Button>("Short life");
        shortl->add_css_class("ghost");
        shortl->add_css_class("small");
        const bool busy = m_td_worker.joinable() || door_busy();
        shortl->set_sensitive(!m_pf_red && !busy && m_door);
        shortl->set_tooltip_text(m_pf_red ? "Preflight has a red check -- see Preflight."
                                          : "Move this save to ~/Closet with 30 days to live -- then offered to the Trash "
                                            "at a review, never deleted unasked. One step; Undo takes it back.");
        shortl->signal_clicked().connect([this] { Glib::signal_idle().connect_once([this] { on_versions_short_life(); }); });
        acts->append(*shortl);
        c->append(*acts);
        cols->append(*c);
    }
    m_vv_box.append(*cols);

    // ── the same bytes? (only worth reading when the sizes match) ──
    if (!L.package && !R.package && L.bytes == R.bytes && vs::same_bytes(la, ra))
        m_vv_box.append(*vtext("The same bytes as the keeper -- a copy, not a version.", "pf-amber"));

    // ── the content ──
    const cm::Kind kind = cm::kind_of(la, f->ext, L.package);
    if (kind == cm::Kind::Image) {
        const auto load = [&](const vs::Member& m, const std::string& path) -> Glib::RefPtr<Gdk::Texture> {
            if (m.bytes > kPictureCap) return {};
            try {
                return Gdk::Texture::create_from_file(Gio::File::create_for_path(path));
            } catch (const Glib::Error& e) {
                if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: viewer: picture '{}' not shown: {}", path, e.what());
            } catch (const std::exception& e) {
                if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: viewer: picture '{}' not shown: {}", path, e.what());
            }
            return {};
        };
        const auto lt = load(L, la), rt = load(R, ra);
        int mw = 1, mh = 1;
        for (const auto& t : {lt, rt})
            if (t) { mw = std::max(mw, t->get_width()); mh = std::max(mh, t->get_height()); }
        const double sc = std::min({1.0, static_cast<double>(kPaneW) / mw, static_cast<double>(kPaneH) / mh});
        auto pics = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 24);
        pics->set_homogeneous(true);
        pics->set_valign(Gtk::Align::START);
        for (const auto& [t, m] : {std::pair{lt, &L}, std::pair{rt, &R}}) {
            auto col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
            auto frame = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
            frame->add_css_class("vv-pane");
            frame->set_size_request(-1, static_cast<int>(mh * sc) + 16);
            frame->set_valign(Gtk::Align::START);
            frame->set_vexpand(false);
            if (t) {
                auto pic = Gtk::make_managed<Gtk::Picture>();
                pic->set_paintable(t);
                pic->set_can_shrink(true);
                pic->set_content_fit(Gtk::ContentFit::FILL);
                pic->set_size_request(std::max(1, static_cast<int>(t->get_width() * sc)),
                                      std::max(1, static_cast<int>(t->get_height() * sc)));
                pic->set_halign(Gtk::Align::CENTER);
                pic->set_valign(Gtk::Align::CENTER);
                frame->append(*pic);
                col->append(*frame);
                col->append(*vtext(commas(static_cast<std::uint64_t>(t->get_width())) + " × " +
                                   commas(static_cast<std::uint64_t>(t->get_height())) + " px" +
                                   (sc < 1.0 ? "  ·  both at " + std::to_string(static_cast<int>(sc * 100 + 0.5)) + "%" : std::string()),
                                   "dim", 0.5f));
            } else {
                auto l = vtext(m->bytes > kPictureCap ? "Too big to show here -- Open shows it."
                                                      : "Rouster can't draw this picture -- Open shows it.", "dim", 0.5f);
                l->set_valign(Gtk::Align::CENTER);
                frame->append(*l);
                col->append(*frame);
            }
            pics->append(*col);
        }
        m_vv_box.append(*pics);
    } else if (kind == cm::Kind::Text) {
        const auto lt = cm::read_text(la, f->ext), rt = cm::read_text(ra, f->ext);
        if (!lt.ok || !rt.ok) {
            m_vv_box.append(*vtext("Can't read the words: " + (!lt.ok ? "the keeper is " + lt.why : "this version is " + rt.why) +
                                   ". Open shows it.", "pf-amber"));
        } else {
            const auto rs = cm::rows(lt.lines, rt.lines, m_vv_all ? 0 : 3);
            auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            std::string sum;
            if (rs.too_different) sum = "Too different to mark line by line -- both shown as they are.";
            else if (rs.identical()) sum = "The same text, word for word" + std::string(L.bytes != R.bytes ? " (the files differ, the words don't)." : ".");
            else {
                std::vector<std::string> parts;
                if (rs.changed) parts.push_back(plural(rs.changed, "line changed", "lines changed"));
                if (rs.removed) parts.push_back(plural(rs.removed, "line", "lines") + " only in the keeper");
                if (rs.added) parts.push_back(plural(rs.added, "line", "lines") + " only in this one");
                parts.push_back(plural(rs.same, "line", "lines") + " the same");
                for (std::size_t x = 0; x < parts.size(); ++x) sum += (x ? " · " : "") + parts[x];
            }
            if (lt.cut || rt.cut) sum += " (Only the first " + commas(cm::kLineCap) + " lines are read.)";
            auto sl = vtext(sum, "detail-path");
            sl->set_wrap(true);
            sl->set_hexpand(true);
            bar->append(*sl);
            if (!rs.too_different && !rs.identical()) {
                auto all = Gtk::make_managed<Gtk::Button>(m_vv_all ? "Only the changes" : "The whole text");
                all->add_css_class("ghost");
                all->add_css_class("small");
                all->signal_clicked().connect([this] {
                    Glib::signal_idle().connect_once([this] { m_vv_all = !m_vv_all; paint_versions_viewer(); });
                });
                bar->append(*all);
            }
            m_vv_box.append(*bar);
            if (!rs.identical() || m_vv_all) {
                auto grid = Gtk::make_managed<Gtk::Grid>();
                grid->add_css_class("vv-text");
                grid->set_column_homogeneous(true);
                grid->set_column_spacing(24);
                grid->set_row_spacing(1);
                int y = 0;
                for (const auto& r : rs.rows) {
                    if (static_cast<std::size_t>(y) >= kRowsShown) break;
                    switch (r.mark) {
                    case cm::Mark::Gap: {
                        auto g = vtext("· " + plural(r.skipped, "line", "lines") + " the same ·", "vv-gap", 0.5f);
                        grid->attach(*g, 0, y, 2, 1);
                        break;
                    }
                    case cm::Mark::Same:
                        grid->attach(*row_label(r.left, rs.too_different ? nullptr : "vv-same"), 0, y);
                        grid->attach(*row_label(r.right, rs.too_different ? nullptr : "vv-same"), 1, y);
                        break;
                    case cm::Mark::Changed:
                        grid->attach(*row_label(r.left, "vv-left"), 0, y);
                        grid->attach(*row_label(r.right, "vv-right"), 1, y);
                        break;
                    case cm::Mark::Removed:
                        grid->attach(*row_label(r.left, "vv-left"), 0, y);
                        grid->attach(*row_label("", "vv-blank"), 1, y);
                        break;
                    case cm::Mark::Added:
                        grid->attach(*row_label("", "vv-blank"), 0, y);
                        grid->attach(*row_label(r.right, "vv-right"), 1, y);
                        break;
                    }
                    ++y;
                }
                m_vv_box.append(*grid);
                if (rs.rows.size() > kRowsShown)
                    m_vv_box.append(*vtext("... and " + plural(rs.rows.size() - kRowsShown, "more row", "more rows") +
                                           " -- Open shows the rest.", "dim"));
            }
        }
    } else {
        auto l = vtext(kind == cm::Kind::App
                           ? "A " + app + " " + (app == "jot" ? "notebook" : app == "Folio" ? "book" : "project") +
                                 " -- Rouster doesn't draw it. The dates and sizes are above; Open in " + app +
                                 " shows each one."
                           : "Rouster can't show this kind of file. The dates and sizes are above; Open shows it in its own app.",
                       "dim");
        l->set_wrap(true);
        m_vv_box.append(*l);
    }
    if (auto* sw = dynamic_cast<Gtk::ScrolledWindow*>(m_vv_box.get_ancestor(Gtk::ScrolledWindow::get_type())))
        Glib::signal_idle().connect_once([sw] { sw->get_vadjustment()->set_value(0.0); });
}

void Shell::on_versions_short_life() {  // handler: s017b -- the shown version -> ~/Closet, one door step
    namespace td = core::tidy;
    if (!m_door || door_busy() || m_td_worker.joinable()) return;
    if (m_td_states.get_visible_child_name() != "viewer") return;   // preview first: only from the viewer
    paint_preflight();
    if (m_pf_red) { m_vv_said = "Preflight has a red check -- nothing moved. See Preflight."; paint_versions_viewer(); return; }
    vs::Family* f = nullptr;
    for (auto& x : m_vf_shown.families)
        if (x.key() == m_vv_key) f = &x;
    if (!f) return;
    const std::size_t k = versions_keeper(*f);
    std::size_t at = f->members.size();
    for (std::size_t x = 0; x < f->members.size(); ++x)
        if (x != k && f->members[x].rel == m_vv_rel) at = x;
    if (at == f->members.size()) return;
    const vs::Member m = f->members[at];
    td::Row row;
    row.rel = m.rel;
    row.bytes = m.bytes;
    row.last_used = m.changed;
    row.query = "versions";
    row.package = m.package;
    row.dev = m.dev;
    row.ino = m.ino;
    row.changed = m.changed;
    td::Act act = td::plan(m_home, td::Verdict::Short, {row});
    if (auto lg = log::get(log::Area::Tidy)) {
        lg->info("versions: viewer: short life '{}': {} moves, {} actions", m.rel, act.moves, act.plan.actions.size());
        for (const auto& d : act.dropped) lg->info("  left out: {}", d);
    }
    if (!act.moves) {
        m_vv_said = act.dropped.empty() ? std::string("Nothing to do.") : act.dropped.front() + " -- Look again.";
        paint_versions_viewer();
        return;
    }
    act.plan.previewed = true;   // the viewer on screen was the preview
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    const auto r = m_door->run(act.plan);
    const std::size_t remembered = td::remember(data_dir(), m_home, act, now);
    if (auto lg = log::get(log::Area::Tidy)) {
        lg->info("versions: viewer: short life: {} done, {} failed; {} remembered", r.done, r.failed, remembered);
        for (const auto& msg : r.messages) lg->warn("  {}", msg);
    }
    if (!r.ok) {
        m_vv_said = "Not moved: " + (r.messages.empty() ? std::string("the door refused it") : r.messages.front()) +
                    (r.done ? " -- Undo puts back what moved." : "");
        paint_activity();
        paint_versions_viewer();
        return;
    }
    // Off the screen's family: the next one along takes its place on the right.
    std::string next;
    for (std::size_t x = at + 1; x < f->members.size() && next.empty(); ++x) if (x != k) next = f->members[x].rel;
    for (std::size_t x = at; x-- > 0 && next.empty();) if (x != k) next = f->members[x].rel;
    if (!m.in_versions && f->loose) {
        --f->loose;
        f->loose_bytes -= std::min(f->loose_bytes, m.bytes);
        if (m_vf_shown.loose) --m_vf_shown.loose;
        m_vf_shown.loose_bytes -= std::min(m_vf_shown.loose_bytes, m.bytes);
    }
    f->members.erase(f->members.begin() + static_cast<long>(at));
    if (f->keeper > at) --f->keeper;
    else if (f->keeper == at) f->keeper = 0;   // not reached: the keeper is never on the right
    m_vv_rel = next;
    m_vv_moved = true;
    m_vv_said = name_of(m.rel) + " is in ~/Closet now, with 30 days to live. Undo (Activity) takes it back.";
    if (f->members.size() < 2) {
        const std::string key = m_vv_key;
        m_vf_shown.families.erase(std::remove_if(m_vf_shown.families.begin(), m_vf_shown.families.end(),
                                                 [&](const vs::Family& x) { return x.key() == key; }),
                                  m_vf_shown.families.end());
        m_vf_checked.erase(key);
        m_vf_keep.erase(key);
    }
    paint_activity();
    paint_versions_viewer();
}

void Shell::versions_open_file(const std::string& rel, const std::string& app) {  // helper: s017b -- Open / Open in Curvz
    const std::string path = m_home + "/" + rel;
    std::string said;
    try {
        if (!app.empty()) {
            Glib::RefPtr<Gio::AppInfo> found;
            const std::string want = lower(app);
            for (const auto& info : Gio::AppInfo::get_all()) {
                if (!info) continue;
                const std::string id = lower(info->get_id());
                if (lower(info->get_name()) == want || id.find("." + want + ".desktop") != std::string::npos ||
                    id == want + ".desktop") { found = info; break; }
            }
            if (!found) said = app + " isn't installed here -- no desktop entry for it.";
            else if (!found->launch(Gio::File::create_for_path(path))) said = app + " didn't start.";
        } else {
            Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(path));
        }
    } catch (const Glib::Error& e) {
        said = "Couldn't open it: " + std::string(e.what());
    }
    if (auto lg = log::get(log::Area::Tidy)) lg->info("versions: viewer: open '{}'{}: {}", rel, app.empty() ? "" : " in " + app,
                                                      said.empty() ? "asked" : said);
    if (!said.empty()) { m_vv_said = said; paint_versions_viewer(); }
}

}  // namespace rouster
