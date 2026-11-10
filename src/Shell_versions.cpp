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

#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

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
        c->append(*vtext("App history · " + commas(r.history.size()), "card-title"));
        auto sub = vtext("Saves an app keeps for itself -- it prunes them on its own schedule. Recognised and left alone; "
                         "never a family.", "card-sub");
        sub->set_wrap(true);
        c->append(*sub);
        for (const auto& h : r.history) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->set_margin_start(6);
            const std::string what = h.app == "Trails" ? "Trails' recordings"
                                   : h.app + "'s history of " + h.what;
            auto l = vtext(what, "detail-path");
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(h.path);
            row->append(*l);
            const char* unit = h.app == "jot" ? " days" : h.app == "Trails" ? " recordings" : " versions";
            row->append(*vtext(commas(h.count) + unit + (h.newest ? ", newest " + ago(h.newest, now) : ""), "dim", 1.0f));
            auto sz = vtext(core::scan::human_bytes(h.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            row->append(*sz);
            auto ch = vtext("managed by " + h.app, "td-query", 0.5f);
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

}  // namespace rouster
