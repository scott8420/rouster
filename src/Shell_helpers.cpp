#include <map>
#include "Shell.hpp"
#include "core/Recents.hpp"
#include "core/Preflight.hpp"
#include "core/Helper.hpp"
#include "core/Settings.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/switch.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/expander.h>
#include <gdkmm/clipboard.h>
#include <gtkmm/image.h>
#include <gtkmm/flowboxchild.h>
#include <glibmm/main.h>
#include <ctime>
#include <glibmm/markup.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>

// Shell_helpers.cpp -- HELPERS. Reusable orchestration called from zones,
// bindings, and handlers: the scan worker and its report, and the recents seam.

namespace rouster {

namespace scan = core::scan;

namespace {
std::string with_commas(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}
std::string esc(const std::string& s) { return Glib::Markup::escape_text(s); }
}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The scan worker (s002). One worker thread owns the walk (which fans out to
// its own pool inside core::scan::scan); the UI polls the atomic counters on a
// timeout and hears "done" through a Glib::Dispatcher -- no GTK call ever
// happens off the UI thread.
// ─────────────────────────────────────────────────────────────────────────────

std::string Shell::home_dir() const {  // helper: $ROUSTER_HOME, else the real home
    // ROUSTER_HOME points the app at a fixture (rouster_selftest --make-fixture
    // DIR) for an Xvfb drive, so no drive ever runs over the real home first.
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h) return h;
    return Glib::get_home_dir();
}

void Shell::start_scan(const std::string& root) {  // helper: spawn the worker
    if (m_worker.joinable()) return;   // one scan at a time; Stop first
    m_progress = std::make_shared<scan::Progress>();
    m_run_big.set_text("0 B");
    m_run_caption.set_text("Looking through " + root);
    set_scanning(true);
    if (auto lg = log::get(log::Area::Scan)) lg->info("scan '{}' (home '{}')", root, m_home);

    m_worker = std::thread([this, prog = m_progress, home = m_home, root, data = data_dir()] {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<scan::Seen> mine;   // s015b: your files, for the catalog
        scan::Report r = scan::scan(home, root, *prog, 0, &mine);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Scan))
            lg->info("done '{}': {} files, {} dirs, {} in {} ms{}{}", root, prog->files.load(),
                     r.dirs, scan::human_bytes(r.total_bytes()), ms,
                     r.cancelled ? " (cancelled)" : "", r.refused.empty() ? "" : " REFUSED: " + r.refused);
        if (auto lg = log::get(log::Area::Scan))   // s015a
            for (const auto& pk : r.packages)
                lg->info("package (kept whole): {} -- {}, {} files", pk.rel, scan::human_bytes(pk.bytes), pk.files);
        // s015b: the catalog remembers the Look -- only a whole, uncancelled one.
        core::catalog::Delta cat;
        if (r.refused.empty() && !r.cancelled) {
            const auto c0 = std::chrono::steady_clock::now();
            std::string root_rel = std::filesystem::path(r.root).lexically_relative(r.home).string();
            if (root_rel == ".") root_rel.clear();
            std::vector<std::string> not_walked = r.unwalked;
            not_walked.insert(not_walked.end(), r.skipped.begin(), r.skipped.end());
            cat = core::catalog::record(data, r.home, root_rel, mine, not_walked,
                                        static_cast<std::int64_t>(std::time(nullptr)));
            const auto cms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - c0).count();
            if (auto lg = log::get(log::Area::Catalog)) {
                if (!cat.set_aside.empty()) lg->warn("set aside: {}", cat.set_aside);
                if (cat.ok)
                    lg->info("look {} '{}': {} files ({} packages), {} new, {} changed, {} read, {} moved, {} gone, "
                             "{} returned; {} rows; atime {}; {} ms", cat.look, root_rel.empty() ? "~" : root_rel,
                             cat.files, cat.packages, cat.fresh, cat.changed, cat.read, cat.moved, cat.gone,
                             cat.returned, cat.rows, cat.atime_mode.empty() ? "?" : cat.atime_mode, cms);
                else lg->warn("not recorded: {}", cat.error);
            }
        }
        {
            std::lock_guard lk(m_report_mx);
            m_report = std::move(r);
            m_cat = std::move(cat);
        }
        m_scan_finished.emit();
    });
    m_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_progress), 120);
}

void Shell::stop_scan() {  // helper: cancel + join (close, dtor)
    if (m_progress) m_progress->cancel = true;
    if (m_worker.joinable()) m_worker.join();
    m_tick.disconnect();
}

bool Shell::tick_progress() {  // helper: the big live counters
    if (!m_progress) return false;
    m_run_big.set_text(scan::human_bytes(m_progress->bytes.load()));
    m_run_caption.set_text(m_progress->cancel.load()
        ? "Stopping…"
        : with_commas(m_progress->files.load()) + " files in " + with_commas(m_progress->dirs.load()) +
          " folders looked at");
    return true;
}

void Shell::set_scanning(bool on) {  // helper: which scan state shows; actions follow
    if (m_act_scan)   m_act_scan->set_enabled(!on);
    if (m_act_pick)   m_act_pick->set_enabled(!on);
    if (m_act_cancel) m_act_cancel->set_enabled(on);
    if (on) { m_spinner.start(); m_scan_states.set_visible_child("running"); }
    else      m_spinner.stop();
}

namespace {
const char* kind_css(scan::Kind k) {
    switch (k) {
        case scan::Kind::Regenerable: return "kind-regenerable";
        case scan::Kind::Trash:       return "kind-trash";
        case scan::Kind::State:       return "kind-state";
        case scan::Kind::Settings:    return "kind-settings";
        case scan::Kind::AppData:     return "kind-appdata";
        case scan::Kind::Protected:   return "kind-protected";
        case scan::Kind::Other:       return "kind-other";
    }
    return "kind-other";
}
constexpr scan::Kind kOrder[] = {scan::Kind::Regenerable, scan::Kind::Trash, scan::Kind::State,
                                 scan::Kind::Settings, scan::Kind::AppData, scan::Kind::Protected,
                                 scan::Kind::Other};
Gtk::Label* text(const std::string& s, const char* css, float x = 0.0f) {
    auto l = Gtk::make_managed<Gtk::Label>(s);
    if (css) l->add_css_class(css);
    l->set_xalign(x);
    return l;
}
}  // namespace

void Shell::paint_report() {  // helper: headline + one card per kind
    {
        std::lock_guard lk(m_report_mx);
        m_shown = m_report;
        m_cat_shown = m_cat;
    }
    const scan::Report& r = m_shown;
    if (!r.refused.empty()) {
        m_scan_sub.set_text("Not scanned: " + r.refused);
        m_scan_states.set_visible_child("start");
        return;
    }
    const auto kb = [&](scan::Kind k) { return r.by_kind[static_cast<int>(k)]; };
    std::uint64_t files = 0;
    for (const auto& b : r.by_kind) files += b.files;

    m_res_big.set_text(scan::human_bytes(kb(scan::Kind::Regenerable).bytes));
    m_res_caption.set_text(std::string("regenerable — caches and build output that rebuild themselves") +
                           (kb(scan::Kind::Trash).bytes
                                ? ".  Plus " + scan::human_bytes(kb(scan::Kind::Trash).bytes) + " already in the Trash."
                                : "."));
    std::string where = "Scanned " + r.root + " · " + scan::human_bytes(r.total_bytes()) + " in " +
                        with_commas(files) + " files";
    if (r.cancelled) where += " · stopped early, partial";
    if (r.errors) where += " · " + with_commas(r.errors) + " unreadable";
    if (!r.packages.empty())
        where += " · " + with_commas(r.packages.size()) + (r.packages.size() == 1 ? " package" : " packages") +
                 " kept whole";
    if (!r.skipped.empty()) {
        where += " · not entered (other filesystems): ";
        for (std::size_t i = 0; i < r.skipped.size() && i < 6; ++i) where += (i ? ", ~/" : "~/") + r.skipped[i];
        if (r.skipped.size() > 6) where += ", …";
    }
    where += ".  Nothing was changed.";
    // s015b: what the catalog learned. Its own file, so "nothing was changed" still holds for yours.
    const auto& c = m_cat_shown;
    if (c.ok) {
        if (c.began) where += "\nCatalog started: " + with_commas(c.files) + " of your files remembered";
        else {
            where += "\nCatalog: " + with_commas(c.files) + " of your files";
            std::vector<std::string> bits;
            if (c.fresh)    bits.push_back(with_commas(c.fresh) + " new");
            if (c.changed)  bits.push_back(with_commas(c.changed) + " changed");
            if (c.read)     bits.push_back(with_commas(c.read) + " opened");
            if (c.moved)    bits.push_back(with_commas(c.moved) + " moved");
            if (c.gone)     bits.push_back(with_commas(c.gone) + " gone");
            if (c.returned) bits.push_back(with_commas(c.returned) + " back");
            if (bits.empty()) where += " -- nothing new since the last Look";
            else {
                where += " -- since the last Look: ";
                for (std::size_t i = 0; i < bits.size(); ++i) where += (i ? ", " : "") + bits[i];
            }
        }
        where += ".";
        if (c.atime_mode == "noatime")
            where += "  Home is mounted noatime: \u201clast opened\u201d can't be known here; Tidy will go by last change.";
        if (!c.set_aside.empty()) where += "  The old catalog wouldn't open and was set aside: " + c.set_aside + ".";
    } else if (!c.error.empty()) {
        where += "\nCatalog not updated: " + c.error + ".";
    }
    m_res_where.set_text(where);

    while (auto* c = m_cards.get_first_child()) m_cards.remove(*c);
    for (scan::Kind k : kOrder) {
        const auto b = kb(k);
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class(kind_css(k));
        dot->set_valign(Gtk::Align::CENTER);
        top->append(*dot);
        top->append(*text(scan::kind_name(k), "card-title"));
        card->append(*top);
        card->append(*text(scan::human_bytes(b.bytes), "card-big"));
        card->append(*text(with_commas(b.files) + " files", "card-sub"));
        auto child = Gtk::make_managed<Gtk::FlowBoxChild>();
        child->set_name(std::to_string(static_cast<int>(k)));
        child->set_child(*card);
        m_cards.append(*child);
        if (static_cast<int>(k) == m_sel_kind) m_cards.select_child(*child);
    }
    paint_detail();
    m_scan_states.set_visible_child("results");
}

void Shell::paint_detail() {  // helper: the selected card's biggest folders
    while (auto* c = m_detail.get_first_child()) m_detail.remove(*c);
    const auto k = static_cast<scan::Kind>(m_sel_kind);
    const auto b = m_shown.by_kind[static_cast<int>(k)];
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto dot = Gtk::make_managed<Gtk::Box>();
    dot->add_css_class("dot");
    dot->add_css_class(kind_css(k));
    dot->set_valign(Gtk::Align::CENTER);
    head->append(*dot);
    head->append(*text(std::string(scan::kind_name(k)) + " · " + scan::human_bytes(b.bytes), "card-title"));
    m_detail.append(*head);
    auto why = text(scan::kind_treatment(k), "dim");
    why->set_wrap(true);
    why->set_margin_bottom(6);
    m_detail.append(*why);

    constexpr std::size_t kTop = 25;
    std::size_t shown = 0, more = 0;
    std::uint64_t more_bytes = 0;
    for (const auto& it : m_shown.items) {
        if (it.kind != k) continue;
        if (shown == kTop) { ++more; more_bytes += it.bytes; continue; }
        ++shown;
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
        auto sz = text(scan::human_bytes(it.bytes), "detail-size", 1.0f);
        sz->set_width_chars(9);
        auto path = text("~/" + it.rel, "detail-path");
        path->set_hexpand(true);
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        path->set_tooltip_text(m_shown.home + "/" + it.rel + "  —  " + with_commas(it.files) + " files");
        line->append(*sz);
        line->append(*path);
        m_detail.append(*line);
    }
    if (k == scan::Kind::Other) {
        m_detail.append(*text("Your own files are totalled, not listed -- they are yours to organize.", "dim"));
        // s015a: Mac packages, each one thing -- never looked inside or split.
        if (!m_shown.packages.empty()) {
            std::uint64_t pb = 0;
            for (const auto& pk : m_shown.packages) pb += pk.bytes;
            auto ph = text("Packages, kept whole · " + with_commas(m_shown.packages.size()) + " · " +
                               scan::human_bytes(pb), "card-title");
            ph->set_margin_top(10);
            m_detail.append(*ph);
            auto pw = text("Scrivener projects and other Mac packages are folders here, but each is one thing to "
                           "Rouster: never looked inside, never split, never swept or paired -- only ever moved whole.",
                           "dim");
            pw->set_wrap(true);
            pw->set_margin_bottom(6);
            m_detail.append(*pw);
            std::size_t pshown = 0;
            for (const auto& pk : m_shown.packages) {
                if (pshown++ == kTop) {
                    m_detail.append(*text("… and " + with_commas(m_shown.packages.size() - kTop) + " more", "dim"));
                    break;
                }
                auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
                auto sz = text(scan::human_bytes(pk.bytes), "detail-size", 1.0f);
                sz->set_width_chars(9);
                auto path = text("~/" + pk.rel, "detail-path");
                path->set_hexpand(true);
                path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
                path->set_tooltip_text(m_shown.home + "/" + pk.rel + "  —  one package, " + with_commas(pk.files) +
                                       " files inside");
                line->append(*sz);
                line->append(*path);
                m_detail.append(*line);
            }
        }
    }
    else if (shown == 0)
        m_detail.append(*text("Nothing of this kind here.", "dim"));
    if (more)
        m_detail.append(*text("… and " + with_commas(more) + " more (" + scan::human_bytes(more_bytes) + ")", "dim"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Activity (s004).
// ─────────────────────────────────────────────────────────────────────────────
std::string Shell::data_dir() const {  // helper: where the manifest lives
    // A fixture home keeps its own manifest, so a drive never writes into the
    // real ~/.local/share/rouster.
    if (const char* h = std::getenv("ROUSTER_HOME"); h && *h) return std::string(h) + "/.local/share/rouster";
    return (std::filesystem::path(Glib::get_user_data_dir()) / "rouster").string();
}

void Shell::paint_activity() {  // helper: the manifest's batches, newest first
    namespace dr = core::door;
    if (door_busy()) {   // the act worker owns the door; on_dev_acted repaints
        m_act_said.set_text("A retire / restore is running -- Activity fills in when it is done.");
        if (m_act_undo_action) m_act_undo_action->set_enabled(false);
        return;
    }
    while (auto* c = m_act_list.get_first_child()) m_act_list.remove(*c);
    const bool can = m_door && m_door->can_undo();
    if (m_act_undo_action) m_act_undo_action->set_enabled(can);
    m_act_undo.set_label(can ? "Undo “" + m_door->undo_label() + "”" : "Undo");
    if (!m_door) return;

    for (const auto& u : m_door->unfinished()) {
        auto l = text("Interrupted last time (resolved by looking): " + u, "pf-amber");
        l->set_wrap(true);
        m_act_list.append(*l);
    }
    const auto& batches = m_door->batches();
    if (batches.empty()) {
        m_act_list.append(*text("Nothing done yet.", "hero-caption", 0.5f));
        return;
    }
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    for (auto it = batches.rbegin(); it != batches.rend(); ++it) {
        std::size_t done = 0, failed = 0;
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 3);
        body->set_margin_top(6);
        for (auto id : it->records) {
            const dr::Record* r = m_door->record(id);
            if (!r) continue;
            const char* mark = "";
            switch (r->state) {
                case dr::State::Done:   ++done;   mark = "→"; break;
                case dr::State::Undone:           mark = "↩"; break;
                default:                ++failed; mark = "✗"; break;
            }
            // A created thing's `from` is the door's staging name: show what was made (and, for a copy, from what).
            const auto req = r->verb == dr::Verb::Root ? core::helper::parse(r->to) : std::nullopt;
            const std::string what = r->verb == dr::Verb::Root ? (req ? core::helper::describe(*req) : r->to) +
                                                                     (r->note.empty() ? "" : " -- " + r->note)
                                   : r->verb == dr::Verb::Settings ? std::string("GNOME settings (dconf load /)")
                                   : r->verb == dr::Verb::Copy ? r->src + " → " + r->to : dr::creates(r->verb) ? r->to : r->from;
            auto l = text(std::string(mark) + "  " + dr::verb_name(r->verb) + "  " + what +
                          (r->error.empty() ? "" : "  -- " + r->error), "detail-path");
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(r->verb == dr::Verb::Root ? "as root, through pkexec: " + r->to +
                                                                (r->note.empty() ? "" : "\n" + r->note) +
                                                                "\nUndo closes the step; what ran as root isn't taken back."
                                : r->verb == dr::Verb::Settings
                                    ? "what they were, saved first: " + r->from + "\nwhat was loaded: " + r->src
                                    : (dr::creates(r->verb) ? (r->src.empty() ? std::string("made") : r->src) : r->from) + "\n→ " + r->to);
            body->append(*l);
        }
        const std::string age = core::preflight::human_age(now - it->when);
        std::string state = (age == "just now" ? age : age + " ago") + " · " +
                            (it->undone ? std::string("undone") : std::to_string(done) + " done");
        if (failed) state += ", " + std::to_string(failed) + " failed";
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        card->add_css_class("card");
        auto ex = Gtk::make_managed<Gtk::Expander>();
        ex->set_use_markup(true);
        ex->set_label("<b>" + esc(it->label) + "</b>    <span alpha='65%'>" + esc(state) + "</span>");
        ex->set_child(*body);
        card->append(*ex);
        m_act_list.append(*card);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Preflight (s003). gather() is a handful of syscalls -- fast enough for the UI
// thread. The checklist itself is core::preflight::evaluate(), pure and tested.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::paint_preflight() {  // helper: gather + evaluate + repaint the cards
    namespace pf = core::preflight;
    const pf::Inputs in = pf::gather(m_home);
    const auto checks = pf::evaluate(in);
    while (auto* c = m_pf_rows.get_first_child()) m_pf_rows.remove(*c);

    int green = 0, red = 0;
    for (const auto& c : checks) {
        if (c.status == pf::Status::Green) ++green;
        if (c.status == pf::Status::Red) ++red;
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
        card->add_css_class("card");
        auto icon = Gtk::make_managed<Gtk::Image>();
        icon->set_valign(Gtk::Align::CENTER);
        icon->set_pixel_size(28);
        const char* chip = "";
        switch (c.status) {
            case pf::Status::Green: icon->set_from_icon_name("rouster-ok-symbolic");     icon->add_css_class("pf-green"); chip = "OK";    break;
            case pf::Status::Amber: icon->set_from_icon_name("rouster-warn-symbolic");   icon->add_css_class("pf-amber"); chip = "NOTE";  break;
            case pf::Status::Red:   icon->set_from_icon_name("rouster-stop-symbolic");   icon->add_css_class("pf-red");   chip = "STOP";  break;
        }
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_hexpand(true);
        words->append(*text(c.title, "card-title"));
        auto d = text(c.detail, "card-sub");
        d->set_wrap(true);
        words->append(*d);
        auto ch = text(chip, "status-chip", 0.5f);
        ch->add_css_class(c.status == pf::Status::Green ? "pf-green" : c.status == pf::Status::Amber ? "pf-amber" : "pf-red");
        ch->set_valign(Gtk::Align::CENTER);
        card->append(*icon);
        card->append(*words);
        // s013: the snapshot row can act -- through the door's Root verb, so it's in Activity.
        // A fixture asks for a dry run, so it may try whenever the helper is there.
        const bool fixture = std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME");
        if (c.id == "snapshot" && in.helper && (in.snap_possible || fixture)) {
            auto b = Gtk::make_managed<Gtk::Button>(m_pf_acting ? "Waiting…" : fixture ? "Try (dry run)" : "Take snapshot");
            b->add_css_class("ghost");
            b->set_valign(Gtk::Align::CENTER);
            b->set_sensitive(!door_busy());
            b->set_tooltip_text(fixture ? std::string("A fixture never snapshots: the helper says what it would do, and does nothing.")
                                        : "A read-only btrfs snapshot of " + in.snap_dir.substr(0, in.snap_dir.rfind('/')) +
                                              ", in " + in.snap_dir + ". Rouster keeps its newest " +
                                              std::to_string(core::helper::kKeepDefault) + ". Asks for your password.");
            b->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_snapshot_take));
            card->append(*b);
        }
        card->append(*ch);
        m_pf_rows.append(*card);
        if (auto lg = log::get(log::Area::Preflight))
            lg->info("{} {}: {}", pf::status_name(c.status), c.id, c.detail);
    }
    m_pf_summary.set_text(std::to_string(green) + " of " + std::to_string(checks.size()) + " checks green" +
                          (red ? ", " + std::to_string(red) + " red" : std::string{}) +
                          ". A red check stops Sweep, Duplicates, Dev projects, Archive, filing into a project and Rules until it clears.");
    m_pf_red = red > 0;
    sweep_gate();
    dups_gate();
    archive_gate();
    pj_gate();
    rl_gate();
    system_gate();
}

// ─────────────────────────────────────────────────────────────────────────────
// Sweep (s006). The worker runs core::sweep::find (read-only); the list it
// paints is the preview; checks are kept in m_sw_shown, the UI thread's copy.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::stop_sweep() {  // helper: cancel + join (close, dtor)
    if (m_sw_progress) m_sw_progress->cancel = true;
    if (m_sw_worker.joinable()) m_sw_worker.join();
    m_sw_tick.disconnect();
}

bool Shell::tick_sweep() {  // helper: the live counters while looking
    if (!m_sw_progress) return false;
    m_sw_run_big.set_text(scan::human_bytes(m_sw_progress->bytes.load()));
    m_sw_run_caption.set_text(m_sw_progress->cancel.load()
        ? "Stopping…"
        : with_commas(m_sw_progress->files.load()) + " files in " + with_commas(m_sw_progress->dirs.load()) +
          " folders looked at");
    return true;
}

void Shell::paint_sweep() {  // helper: groups, rows with checks, the Trash line
    namespace sw = core::sweep;
    while (auto* c = m_sw_list.get_first_child()) m_sw_list.remove(*c);
    const auto& items = m_sw_shown.items;

    for (int gi = 0; gi < sw::kGroupCount; ++gi) {
        const auto g = static_cast<sw::Group>(gi);
        std::uint64_t gbytes = 0;
        std::size_t n = 0;
        for (const auto& c : items) if (c.group == g) { gbytes += c.bytes; ++n; }
        if (!n) continue;

        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class(g == sw::Group::Logs ? "kind-state" : "kind-regenerable");
        dot->set_valign(Gtk::Align::CENTER);
        head->append(*dot);
        auto title = text(sw::group_name(g), "card-title");
        title->set_hexpand(true);
        head->append(*title);
        head->append(*text(scan::human_bytes(gbytes) + " · " + with_commas(n) + (n == 1 ? " item" : " items"),
                           "card-sub", 1.0f));
        card->append(*head);
        auto why = text(sw::group_why(g), "dim");
        why->set_wrap(true);
        why->set_margin_bottom(4);
        card->append(*why);

        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& c = items[i];
            if (c.group != g) continue;
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(c.checked);
            cb->signal_toggled().connect([this, cb, i] {
                if (i < m_sw_shown.items.size()) m_sw_shown.items[i].checked = cb->get_active();
                sweep_gate();
            });
            auto sz = text(scan::human_bytes(c.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            auto path = text("~/" + c.rel, "detail-path");
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text(m_home + "/" + c.rel + "  —  " + with_commas(c.files) +
                                   (c.files == 1 ? " file" : " files") + (c.note.empty() ? "" : "\n" + c.note));
            row->append(*cb);
            row->append(*sz);
            row->append(*path);
            if (!c.note.empty()) row->append(*text(c.note, "dim", 1.0f));
            card->append(*row);
        }
        m_sw_list.append(*card);
    }
    if (items.empty())
        m_sw_list.append(*text("Nothing to sweep -- caches and logs are already clean.", "hero-caption", 0.5f));

    // The Trash: measured, never offered (the one irreversible step).
    auto trash = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    trash->add_css_class("detail");
    auto th = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto dot = Gtk::make_managed<Gtk::Box>();
    dot->add_css_class("dot");
    dot->add_css_class("kind-trash");
    dot->set_valign(Gtk::Align::CENTER);
    th->append(*dot);
    th->append(*text("Trash · " + scan::human_bytes(m_sw_shown.trash_bytes) + " in " +
                     with_commas(m_sw_shown.trash_files) + " files", "card-title"));
    trash->append(*th);
    auto tw = text("Already thrown away. Emptying the Trash is the one step that can't be undone, "
                   "so Rouster leaves it to you (Files -> Trash -> Empty).", "dim");
    tw->set_wrap(true);
    trash->append(*tw);
    m_sw_list.append(*trash);

    std::string foot;
    if (m_sw_shown.cancelled) foot += "Stopped early -- the list is partial. ";
    if (m_sw_shown.errors) foot += with_commas(m_sw_shown.errors) + " folders could not be read. ";
    if (const auto& kw = m_sw_shown.kept_whole; !kw.empty()) {   // s015a
        foot += with_commas(kw.size()) + (kw.size() == 1 ? " package" : " packages") +
                " kept whole -- never swept, and no folder holding one is offered: ";
        for (std::size_t i = 0; i < kw.size() && i < 4; ++i) foot += (i ? ", ~/" : "~/") + kw[i];
        if (kw.size() > 4) foot += ", …";
        foot += ". ";
    }
    if (!foot.empty()) m_sw_list.append(*text(foot, "dim"));
    sweep_gate();
}

void Shell::sweep_gate() {  // helper: the act button's label + whether it may act
    std::uint64_t bytes = 0, all = 0;
    std::size_t n = 0;
    for (const auto& c : m_sw_shown.items) {
        all += c.bytes;
        if (c.checked) { bytes += c.bytes; ++n; }
    }
    m_sw_big.set_text(scan::human_bytes(bytes));
    m_sw_caption.set_text("chosen, of " + scan::human_bytes(all) + " found · " + with_commas(n) + " of " +
                          with_commas(m_sw_shown.items.size()) + " items checked");
    m_sw_go.set_label(n ? "Move " + scan::human_bytes(bytes) + " to the Trash" : "Move to the Trash");
    const bool busy = m_sw_worker.joinable();
    if (m_pf_red)
        m_sw_gate.set_text("Preflight has a red check. Sweep waits until it clears -- see Preflight.");
    else if (!n)
        m_sw_gate.set_text("Nothing checked.");
    else
        m_sw_gate.set_text("Everything goes to the Trash as one step; Undo brings it all back.");
    if (m_act_sw_run) m_act_sw_run->set_enabled(n > 0 && !m_pf_red && !busy);
}

// ─────────────────────────────────────────────────────────────────────────────
// Duplicates (s008). The worker runs core::dups::find (read-only: it reads to
// hash, never writes); the cards are the preview; checks and keepers live in
// m_du_shown. Only the first kDupsShown groups are painted, and a group that
// isn't painted is never checked -- nothing acts that wasn't shown.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// System (s013b). The rows are the preview; checks live in m_sy_items.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::stop_system() {  // helper: a run in flight finishes (the door stays whole)
    if (m_sy_worker.joinable()) m_sy_worker.join();
}

void Shell::paint_system() {  // helper: one card per item -- check, size, why, the warning
    namespace hp = core::helper;
    while (auto* c = m_sy_list.get_first_child()) m_sy_list.remove(*c);
    for (std::size_t i = 0; i < m_sy_items.size(); ++i) {
        const auto& it = m_sy_items[i];
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 14);
        card->add_css_class("card");
        auto cb = Gtk::make_managed<Gtk::CheckButton>();
        cb->set_active(it.checked && it.available);
        cb->set_sensitive(it.available);
        cb->set_valign(Gtk::Align::CENTER);
        cb->signal_toggled().connect([this, cb, i] {
            if (i < m_sy_items.size()) m_sy_items[i].checked = cb->get_active();
            system_gate();
        });
        card->append(*cb);
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        words->set_hexpand(true);
        words->append(*text(it.title, "card-title"));
        auto why = text(it.why, "card-sub");
        why->set_wrap(true);
        words->append(*why);
        auto note = text(it.note, hp::irreversible(it.req.op) && it.available ? "sy-warn" : "sy-note");
        note->set_wrap(true);
        words->append(*note);
        card->append(*words);
        if (it.sized) {
            auto sz = text((it.complete ? "" : "at least ") + scan::human_bytes(it.bytes), "card-title", 1.0f);
            sz->set_valign(Gtk::Align::CENTER);
            if (!it.complete) sz->set_tooltip_text("Some of it isn't readable without root; the helper measures it all.");
            card->append(*sz);
        }
        m_sy_list.append(*card);
    }
    std::uint64_t total = 0;
    for (const auto& it : m_sy_items) if (it.sized) total += it.bytes;
    m_sy_caption.set_text(scan::human_bytes(total) + " in the package cache and the journal. Each runs as root, "
                          "after one password; nothing here touches your home.");
    system_gate();
}

void Shell::system_gate() {  // helper: the act button's label + whether it may act
    namespace hp = core::helper;
    const bool fixture = std::getenv("ROUSTER_HOME") && *std::getenv("ROUSTER_HOME");
    std::size_t n = 0;
    std::string gone;   // the checked ones that can't be undone, by name
    for (const auto& it : m_sy_items)
        if (it.checked && it.available) {
            ++n;
            if (hp::irreversible(it.req.op)) {
                std::string t = it.title;
                t[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
                gone += (gone.empty() ? "the " : " and the ") + t;
            }
        }
    m_sy_go.set_label(fixture ? "Try " + std::to_string(n) + " (dry run)"
                              : "Run " + std::to_string(n) + " as root");
    std::string why;
    if (!core::door::helper_installed()) why = "Needs the root helper: ./install.sh --helper (sudo).";
    else if (m_pf_red) why = "A Preflight check is red -- see Preflight.";
    else if (door_busy()) why = "Waiting for another step to finish.";
    else if (!n) why = "Nothing checked.";
    m_sy_go.set_sensitive(why.empty());
    m_sy_gate.set_text(!why.empty() ? why
                       : fixture ? std::string("A fixture never runs root work: the helper says what it would do.")
                       : !gone.empty() ? "Asks for your password. Clearing " + gone + " can't be undone."  
                              : std::string("Asks for your password. A trim changes no file."));
}

void Shell::stop_preflight() {  // helper: a snapshot in flight finishes (the door stays whole)
    if (m_pf_worker.joinable()) m_pf_worker.join();
}

void Shell::stop_dups() {  // helper: cancel + join (close, dtor)
    if (m_du_progress) m_du_progress->cancel = true;
    if (m_du_worker.joinable()) m_du_worker.join();
    m_du_tick.disconnect();
    if (m_du_act_worker.joinable()) m_du_act_worker.join();   // a run in flight finishes; the door stays whole
}

bool Shell::tick_dups() {  // helper: the live counters while looking
    if (!m_du_progress) return false;
    const int st = m_du_progress->stage.load();
    m_du_run_big.set_text(st == 0 ? with_commas(m_du_progress->files.load()) + " files"
                                  : scan::human_bytes(m_du_progress->hashed.load()) + " read");
    m_du_run_caption.set_text(m_du_progress->cancel.load() ? "Stopping…" : core::dups::stage_name(st));
    return true;
}

void Shell::dups_preset() {  // helper: checks for the mode; nothing unshown stays checked
    core::dups::preset(m_du_shown, m_du_mode);
    for (std::size_t gi = kDupsShown; gi < m_du_shown.groups.size(); ++gi)
        for (auto& m : m_du_shown.groups[gi].members) m.checked = false;
}

void Shell::paint_dups() {  // helper: mode switch, one card per group, rows with checks
    namespace du = core::dups;
    while (auto* c = m_du_list.get_first_child()) m_du_list.remove(*c);
    const bool share = m_du_mode == du::Mode::Share;

    // The mode switch.
    auto modes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto b_share = Gtk::make_managed<Gtk::ToggleButton>("Share blocks");
    auto b_trash = Gtk::make_managed<Gtk::ToggleButton>("Move copies to the Trash");
    for (auto* b : {b_share, b_trash}) { b->add_css_class("ghost"); b->add_css_class("mode"); }
    b_trash->set_group(*b_share);
    b_share->set_active(share);
    b_trash->set_active(!share);
    b_share->set_sensitive(m_du_shown.reflink);
    b_share->set_tooltip_text(m_du_shown.reflink
        ? "Every copy keeps its path; their blocks become one. Nothing to put back, nothing to break."
        : "Your home folder isn't on btrfs, so its files can't share blocks.");
    b_trash->set_tooltip_text("A checked copy goes to the Trash. Undo brings it back.");
    b_share->signal_toggled().connect([this, b_share] { if (b_share->get_active()) on_dups_mode(core::dups::Mode::Share); });
    b_trash->signal_toggled().connect([this, b_trash] { if (b_trash->get_active()) on_dups_mode(core::dups::Mode::Trash); });
    modes->append(*b_share);
    modes->append(*b_trash);
    auto mode_why = text(share ? "Copies keep their paths and share one set of blocks -- every copy is pre-checked."
                               : m_du_shown.reflink
                                     ? "Only what you check goes to the Trash. Nothing is pre-checked."
                                     : "Not btrfs here, so copies can't share blocks. Only what you check goes to the Trash.",
                         "dim");
    mode_why->set_wrap(true);
    mode_why->set_hexpand(true);
    modes->append(*mode_why);
    m_du_list.append(*modes);

    const auto date = [](std::int64_t ns) {
        std::time_t t = static_cast<std::time_t>(ns / 1'000'000'000);
        std::tm tm{};
        ::localtime_r(&t, &tm);
        char b[16];
        std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
        return std::string(b);
    };
    const std::size_t n = std::min(m_du_shown.groups.size(), kDupsShown);
    for (std::size_t gi = 0; gi < n; ++gi) {
        const auto& g = m_du_shown.groups[gi];
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class(g.folder ? "du-folder" : "du-file");
        dot->set_valign(Gtk::Align::CENTER);
        head->append(*dot);
        const std::string name = std::filesystem::path(g.members[g.keep].rel).filename().string();
        auto title = text(g.folder ? name + "/  ·  a folder of " + with_commas(g.files) + (g.files == 1 ? " file" : " files")
                                   : name, "card-title");
        title->set_hexpand(true);
        title->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        head->append(*title);
        head->append(*text(scan::human_bytes(g.bytes) + " × " + std::to_string(g.members.size()), "card-sub", 1.0f));
        card->append(*head);

        std::vector<std::size_t> order{g.keep};   // the keeper's row first
        for (std::size_t i = 0; i < g.members.size(); ++i) if (i != g.keep) order.push_back(i);
        for (std::size_t i : order) {
            const auto& m = g.members[i];
            const bool keeper = i == g.keep;
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(!keeper && m.checked);
            cb->set_sensitive(!keeper);
            cb->set_tooltip_text(keeper ? "The keeper -- never acted on" : share ? "Share this copy's blocks with the keeper"
                                                                                 : "Send this copy to the Trash");
            cb->signal_toggled().connect([this, cb, gi, i] {
                if (gi < m_du_shown.groups.size() && i < m_du_shown.groups[gi].members.size())
                    m_du_shown.groups[gi].members[i].checked = cb->get_active();
                dups_gate();
            });
            row->append(*cb);
            auto path = text("~/" + m.rel + (g.folder ? "/" : ""), "detail-path");
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text(m_home + "/" + m.rel);
            row->append(*path);
            row->append(*text(date(m.mtime_ns), "dim", 1.0f));
            if (keeper) {
                auto chip = text("KEEP · " + g.keep_why, "status-chip");
                chip->add_css_class("du-keep");
                row->append(*chip);
            } else {
                if (g.shared(i))
                    row->append(*text(share ? "already shares blocks" : "shares blocks -- frees nothing", "dim", 1.0f));
                auto keep = Gtk::make_managed<Gtk::Button>("Keep this");
                keep->add_css_class("ghost");
                keep->add_css_class("small");
                keep->set_tooltip_text("Make this the one that stays; the others become the copies");
                keep->signal_clicked().connect([this, gi, i] { on_dups_keep(gi, i); });
                row->append(*keep);
            }
            card->append(*row);
        }
        m_du_list.append(*card);
    }
    if (m_du_shown.groups.empty())
        m_du_list.append(*text("No duplicates of 4 KB or more -- every file here is one of a kind.", "hero-caption", 0.5f));

    std::string foot = "Looked at " + with_commas(m_du_shown.files) + " files and read " +
                       scan::human_bytes(m_du_shown.hashed) + " to compare them. Hidden folders, build output and "
                       "anything protected were left out. ";
    if (m_du_shown.groups.size() > kDupsShown)
        foot += with_commas(m_du_shown.groups.size() - kDupsShown) + " smaller groups aren't shown, and stay unchecked. ";
    if (m_du_shown.cancelled) foot += "Stopped early -- the list is partial. ";
    if (m_du_shown.errors) foot += with_commas(m_du_shown.errors) + " things could not be read. ";
    if (const auto& pk = m_du_shown.packages; !pk.empty()) {   // s015a
        foot += with_commas(pk.size()) + (pk.size() == 1 ? " package" : " packages") +
                " kept whole -- never looked inside or paired: ";
        for (std::size_t i = 0; i < pk.size() && i < 4; ++i) foot += (i ? ", ~/" : "~/") + pk[i];
        if (pk.size() > 4) foot += ", …";
        foot += ". ";
    }
    auto f = text(foot, "dim");
    f->set_wrap(true);
    m_du_list.append(*f);
    dups_gate();
}

void Shell::dups_gate() {  // helper: the act button's label + whether it may act
    namespace du = core::dups;
    const bool share = m_du_mode == du::Mode::Share;
    std::uint64_t gain = 0, all = 0;
    std::size_t copies = 0;
    for (const auto& g : m_du_shown.groups) {
        gain += g.gain(m_du_mode);
        all += g.bytes * (g.members.size() - 1);
        for (std::size_t i = 0; i < g.members.size(); ++i)
            if (i != g.keep && g.members[i].checked && !(share && g.shared(i))) ++copies;
    }
    m_du_big.set_text(scan::human_bytes(gain));
    m_du_caption.set_text(std::string(share ? "freed by sharing" : "to the Trash") + ", of " + scan::human_bytes(all) +
                          " in copies · " + with_commas(m_du_shown.groups.size()) + " groups · " +
                          with_commas(copies) + (copies == 1 ? " copy" : " copies") + " checked");
    const std::string nc = with_commas(copies) + (copies == 1 ? " copy" : " copies");
    m_du_go.set_label(!copies ? (share ? "Share blocks" : "Move to the Trash")
                              : share ? "Share blocks of " + nc : "Move " + nc + " to the Trash");
    const bool busy = m_du_worker.joinable() || door_busy();
    if (m_pf_red)
        m_du_gate.set_text("Preflight has a red check. Duplicates waits until it clears -- see Preflight.");
    else if (!copies)
        m_du_gate.set_text("Nothing checked.");
    else if (share)
        m_du_gate.set_text("One step. Every path and every byte stays; the filesystem checks each pair is "
                           "identical before it shares a block.");
    else
        m_du_gate.set_text("Everything goes to the Trash as one step; Undo brings it all back.");
    if (m_act_du_run) m_act_du_run->set_enabled(copies > 0 && !m_pf_red && !busy);
}

// ─────────────────────────────────────────────────────────────────────────────
// Archive (s009). The worker runs core::archive::find (read-only); the stored
// list comes from the manifest on the UI thread (the door is the UI's). Rows
// past kArchiveShown aren't painted and are never checked.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::stop_tidy() {  // helper: cancel + join (close, dtor)
    if (m_td_progress) m_td_progress->cancel = true;
    if (m_td_worker.joinable()) m_td_worker.join();
    m_td_tick.disconnect();
}

void Shell::paint_tidy() {  // helper: s015c -- the report's cards
    namespace td = core::tidy;
    while (auto* c = m_td_list.get_first_child()) m_td_list.remove(*c);
    const auto& r = m_td_shown;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    std::string cap = with_commas(r.files) + " of your files, " + scan::human_bytes(r.bytes) + ". Proposed: ";
    for (std::size_t i = 0; i < r.groups.size(); ++i)
        cap += (i ? ", " : "") + with_commas(r.groups[i].files) + " " +
               (r.groups[i].verdict == td::Verdict::Short ? "short life" :
                r.groups[i].verdict == td::Verdict::Dormant ? "dormant" : "undecided");
    cap += "; " + with_commas(r.active) + " active";
    if (r.kept) cap += ", " + with_commas(r.kept) + " you said to keep where they are";
    if (r.left_alone) cap += ", " + with_commas(r.left_alone) + " left alone (hands off)";
    cap += ". Nothing moves until you press a card's button.";
    if (m_td_cat_shown.ok && !m_td_cat_shown.began &&
        (m_td_cat_shown.fresh || m_td_cat_shown.moved || m_td_cat_shown.gone))
        cap += "  Since the last Look: " + with_commas(m_td_cat_shown.fresh) + " new, " +
               with_commas(m_td_cat_shown.moved) + " moved, " + with_commas(m_td_cat_shown.gone) + " gone.";
    m_td_caption.set_text(cap);

    const auto card = [&](const std::string& title, const std::string& sub) {
        auto c = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        c->add_css_class("card");
        c->append(*text(title, "card-title"));
        if (!sub.empty()) {
            auto s = text(sub, "card-sub");
            s->set_wrap(true);
            c->append(*s);
        }
        m_td_list.append(*c);
        return c;
    };
    const auto line = [&](Gtk::Box* into, const std::string& left, const std::string& mid, const std::string& right,
                          const std::string& chip, const std::string& tip) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->set_margin_start(6);
        auto l = text(left, "detail-path");
        l->set_hexpand(true);
        l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        if (!tip.empty()) l->set_tooltip_text(tip);
        row->append(*l);
        if (!mid.empty()) row->append(*text(mid, "dim", 1.0f));
        if (!right.empty()) {
            auto rt = text(right, "detail-size", 1.0f);
            rt->set_width_chars(9);
            row->append(*rt);
        }
        if (!chip.empty()) {
            auto ch = text(chip, "td-query", 0.5f);
            ch->set_valign(Gtk::Align::CENTER);
            ch->set_tooltip_text(chip == "expired" || chip == "short" || chip == "to sort"   // s016b: the Closet's chips
                                     ? std::string("In the Closet: ") + (chip == "expired" ? "past its 30 days"
                                                                       : chip == "short" ? "short life, ageing"
                                                                                         : "waiting to be sorted")
                                     : "Proposed by the \u201c" + chip + "\u201d query");
            row->append(*ch);
        }
        into->append(*row);
    };

    // s016: the Closet -- made, and where Downloads land. Its card lists every step (the preview).
    {
        const auto st = td::closet_state(m_home);
        if (!st.closet || !st.junk || !st.redirected) {
            auto c = card(st.closet ? "The Closet" : "The Closet \u00b7 not made yet",
                          "~/Closet is where new files land and strays wait to be sorted; ~/Closet/Junk Drawer keeps "
                          "things worth keeping that have no home yet (they never age out).");
            if (!st.redirected) {
                auto cb = Gtk::make_managed<Gtk::CheckButton>(
                    "Downloads land in the Closet (now ~/" + (st.download.empty() ? std::string("Downloads") : st.download) + ")");
                cb->set_active(m_td_redirect);
                cb->signal_toggled().connect([this, cb] {
                    m_td_redirect = cb->get_active();
                    Glib::signal_idle().connect_once([this] { paint_tidy(); });   // not from inside the row's own signal
                });
                c->append(*cb);
            }
            const auto plan = td::closet_plan(m_home, m_td_redirect);
            for (const auto& a : plan.actions) {
                const std::string where = "~/" + (a.verb == core::door::Verb::Trash ? a.from : a.to).substr(m_home.size() + 1);
                const std::string verb = a.verb == core::door::Verb::MakeDir ? "make" :
                                         a.verb == core::door::Verb::Trash ? "to the Trash" : "write";
                line(c, where, a.reason, "", verb, "");
            }
            if (!st.redirected && m_td_redirect) {
                auto n = text("Screenshots can't be pointed: GNOME saves them into Pictures/Screenshots itself. "
                              "The \u201cscreenshots\u201d query gathers the old ones instead.", "dim");
                n->set_wrap(true);
                c->append(*n);
            }
            auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            auto gate = text(m_pf_red ? "Preflight has a red check -- see Preflight." : "One step; Undo takes it back.", "dim");
            gate->set_hexpand(true);
            gate->set_wrap(true);
            bar->append(*gate);
            auto go = Gtk::make_managed<Gtk::Button>(!st.closet ? "Make the Closet" : "Point Downloads at the Closet");
            go->add_css_class("act");
            go->set_sensitive(!plan.actions.empty() && !m_pf_red && !door_busy());
            go->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_tidy_closet));
            bar->append(*go);
            c->append(*bar);
        }
    }

    if (!m_td_keep_said.empty()) {
        auto k = text(m_td_keep_said, "dim");
        k->set_wrap(true);
        k->set_margin_start(16);
        m_td_list.append(*k);
    }
    // s016b: the Closet's own card -- what's in it, each short-life item's expiry; the review.
    m_tc_cbs.clear();
    m_tc_all = nullptr; m_tc_gate = nullptr; m_tc_trash = nullptr; m_tc_junk = nullptr;
    m_tc_view = td::closet(data_dir(), m_home, now);
    {
        std::set<std::string> still;
        for (const auto& it : m_tc_view.items) if (m_tc_checked.count(it.rel)) still.insert(it.rel);
        m_tc_checked.swap(still);
    }
    if (m_tc_view.closet) {
        const auto& v = m_tc_view;
        std::string head = "In the Closet \u00b7 " + with_commas(v.items.size());
        if (!v.items.empty()) head += " \u00b7 " + scan::human_bytes(v.bytes);
        if (v.expired) head += " \u00b7 " + with_commas(v.expired) + " expired";
        auto c = card(head, "Short-life files wait here 30 days, then come up for the Trash -- only when you check them. "
                            "The rest wait to be sorted, or go to the Junk Drawer: kept, no home yet, never ages out." +
                            std::string(v.junk ? " (" + with_commas(v.junk) + " in the Junk Drawer now.)" : ""));
        if (v.items.empty()) {
            c->append(*text("Nothing waiting.", "dim"));
        } else {
            auto all = Gtk::make_managed<Gtk::CheckButton>("Select all \u00b7 " + with_commas(v.items.size()));
            all->set_margin_top(4);
            all->signal_toggled().connect([this, all] {
                if (m_td_bulk) return;
                const bool on = all->get_active() || all->get_inconsistent();
                m_td_bulk = true;
                all->set_inconsistent(false);
                all->set_active(on);
                for (auto& [b, rel] : m_tc_cbs) b->set_active(on);
                m_td_bulk = false;
                closet_gate();
            });
            c->append(*all);
            m_tc_all = all;
            auto rows_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
            sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
            sc->set_min_content_height(static_cast<int>(std::min<std::size_t>(v.items.size() * 25, 420)));
            sc->set_max_content_height(420);
            sc->set_propagate_natural_height(true);
            rows_box->set_margin_end(14);
            sc->set_child(*rows_box);
            c->append(*sc);
            for (const auto& it : v.items) {
                auto rw = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                cb->set_active(m_tc_checked.count(it.rel) > 0);
                const std::string rel = it.rel;
                cb->signal_toggled().connect([this, cb, rel] {
                    if (cb->get_active()) m_tc_checked.insert(rel); else m_tc_checked.erase(rel);
                    if (!m_td_bulk) closet_gate();
                });
                m_tc_cbs.emplace_back(cb, rel);
                rw->append(*cb);
                auto inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
                inner->set_hexpand(true);
                const bool ex = td::expired(it, now);
                const std::string when =
                    ex ? "expired " + td::age(now - it.expires) + " ago"
                  : it.expires ? "expires in " + td::age(it.expires - now)
                  : !it.arrived ? "new -- Look again to date it"
                  : now - it.arrived < 86400 ? "arrived today"
                                             : "here " + td::age(now - it.arrived);
                line(inner, "~/" + it.rel + (it.dir ? "/" : ""), when, scan::human_bytes(it.bytes),
                     ex ? "expired" : it.expires ? "short" : "to sort", ex ? "Past its 30 days: the Trash, if you check it"
                                                                 : "~/" + it.rel);
                rw->append(*inner);
                rows_box->append(*rw);
            }
        }
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        bar->set_margin_top(6);
        auto gate = text("", "dim");
        gate->set_hexpand(true);
        gate->set_wrap(true);
        bar->append(*gate);
        auto junk = Gtk::make_managed<Gtk::Button>("To the Junk Drawer");
        junk->add_css_class("ghost");
        junk->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_closet_junk));
        bar->append(*junk);
        auto trash = Gtk::make_managed<Gtk::Button>("Move to the Trash");
        trash->add_css_class("act");
        trash->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_closet_trash));
        bar->append(*trash);
        if (!v.items.empty()) { c->append(*bar); m_tc_gate = gate; m_tc_trash = trash; m_tc_junk = junk; }
    }

    // s016a2: hands off -- folders Rouster leaves alone (Tidy, Archive, Duplicates).
    {
        auto c = card("Hands off \u00b7 " + with_commas(r.hands_off.size()) +
                      (r.left_alone ? " \u00b7 " + with_commas(r.left_alone) + " files, " + scan::human_bytes(r.left_alone_bytes) : ""),
                      "Folders you said to leave alone -- your novels, covers and workups. Nothing inside one is ever "
                      "proposed by Tidy, Archive or Duplicates. Forget takes the mark off.");
        for (const auto& rel : r.hands_off) {
            auto rw = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            auto l = text("~/" + rel, "detail-path");
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(m_home + "/" + rel);
            rw->append(*l);
            auto forget = Gtk::make_managed<Gtk::Button>("Forget");
            forget->add_css_class("ghost");
            forget->add_css_class("small");
            forget->signal_clicked().connect([this, rel] {
                Glib::signal_idle().connect_once([this, rel] { on_tidy_forget(rel); });   // not from inside the row's own signal
            });
            rw->append(*forget);
            c->append(*rw);
        }
        if (!m_td_said.empty()) { auto s = text(m_td_said, "dim"); s->set_wrap(true); c->append(*s); }
        auto mark = Gtk::make_managed<Gtk::Button>("Leave a folder alone\u2026");
        mark->add_css_class("ghost");
        mark->set_halign(Gtk::Align::END);
        mark->signal_clicked().connect(sigc::mem_fun(*this, &Shell::on_tidy_hands_off));
        c->append(*mark);
    }

    // The verdict cards: a check per painted row, the destination in its tooltip, one act button each.
    m_td_painted.clear();
    m_td_go.fill(nullptr);
    m_td_gates.fill(nullptr);
    m_td_all.fill(nullptr);
    for (auto& v : m_td_cbs) v.clear();
    for (const auto& g : r.groups) {
        const auto vi = static_cast<std::size_t>(g.verdict);
        auto c = card(std::string(td::verdict_name(g.verdict)) + " \u00b7 " + with_commas(g.files) + " \u00b7 " +
                      scan::human_bytes(g.bytes), td::verdict_why(g.verdict));
        if (g.rows.empty()) { c->append(*text("Nothing proposed.", "dim")); continue; }
        std::vector<td::Row> shown(g.rows.begin(), g.rows.begin() + static_cast<std::ptrdiff_t>(std::min(g.rows.size(), kTidyShown)));
        const auto dest = td::plan(m_home, g.verdict, shown);   // read-only: where each would go, for the tooltips
        std::map<std::string, std::string> to;
        for (const auto& a : dest.plan.actions)
            if (a.verb == core::door::Verb::Move) to[a.from.substr(m_home.size() + 1)] = a.to.substr(m_home.size() + 1);
        // s016a3: "Select all" over the offered rows, then every row in a scroller.
        auto all = Gtk::make_managed<Gtk::CheckButton>("Select all \u00b7 " + with_commas(shown.size()));
        all->set_margin_top(4);
        all->signal_toggled().connect([this, vi, all] {
            if (m_td_bulk) return;
            const bool on = all->get_active() || all->get_inconsistent();
            m_td_bulk = true;
            all->set_inconsistent(false);
            all->set_active(on);
            for (auto* b : m_td_cbs[vi]) b->set_active(on);   // each row's handler updates m_td_checked
            m_td_bulk = false;
            tidy_gate();
        });
        c->append(*all);
        m_td_all[vi] = all;
        auto rows_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
        sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        // Inside the page's own scroller natural height isn't propagated: ask for the rows' height, at most 420.
        sc->set_min_content_height(static_cast<int>(std::min<std::size_t>(shown.size() * 25, 420)));
        sc->set_max_content_height(420);
        sc->set_propagate_natural_height(true);
        rows_box->set_margin_end(14);   // the overlay scrollbar stays off the chips
        sc->set_child(*rows_box);
        c->append(*sc);
        for (const auto& row : shown) {
            m_td_painted.insert(row.rel);
            auto rw = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(m_td_checked.count(row.rel) > 0);
            const auto it = to.find(row.rel);
            cb->set_sensitive(it != to.end());
            const std::string rel = row.rel;
            cb->signal_toggled().connect([this, cb, rel] {
                if (cb->get_active()) m_td_checked.insert(rel); else m_td_checked.erase(rel);
                if (!m_td_bulk) tidy_gate();
            });
            if (it != to.end()) m_td_cbs[vi].push_back(cb);
            rw->append(*cb);
            auto inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
            inner->set_hexpand(true);
            line(inner, "~/" + row.rel + (row.package ? "  (package, whole)" : ""),
                 !row.last_used ? "" : now - row.last_used < 86400 ? "used today" : "unused " + td::age(now - row.last_used),
                 scan::human_bytes(row.bytes), row.query,
                 it != to.end() ? "\u2192 ~/" + it->second : "Not offered: changed since the Look -- Look again");
            rw->append(*inner);
            auto keep = Gtk::make_managed<Gtk::Button>("Keep here");
            keep->add_css_class("ghost");
            keep->add_css_class("small");
            keep->set_valign(Gtk::Align::CENTER);
            keep->set_tooltip_text("Leave it where it is -- never proposed again. Forget (under \u201cKept here\u201d) undoes it.");
            keep->signal_clicked().connect([this, rel] {
                Glib::signal_idle().connect_once([this, rel] { on_tidy_keep(rel, true); });
            });
            rw->append(*keep);
            rows_box->append(*rw);
        }
        if (g.rows.size() > kTidyShown)
            c->append(*text("... and " + with_commas(g.rows.size() - kTidyShown) + " more -- not shown, never checked", "dim"));
        auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        bar->set_margin_top(6);
        auto gate = text("", "dim");
        gate->set_hexpand(true);
        gate->set_wrap(true);
        bar->append(*gate);
        auto go = Gtk::make_managed<Gtk::Button>("Move");
        go->add_css_class("act");
        const auto v = g.verdict;
        go->signal_clicked().connect([this, v] { on_tidy_act(v); });
        bar->append(*go);
        c->append(*bar);
        m_td_go[vi] = go;
        m_td_gates[vi] = gate;
    }

    if (const auto kept = td::kept(data_dir()); !kept.empty()) {   // s016b
        auto c = card("Kept here \u00b7 " + with_commas(kept.size()),
                      "Files you said stay where they are -- never proposed again. Forget lets Tidy propose one again.");
        auto rows_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
        sc->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        sc->set_min_content_height(static_cast<int>(std::min<std::size_t>(kept.size() * 30, 240)));
        sc->set_max_content_height(240);
        sc->set_propagate_natural_height(true);
        rows_box->set_margin_end(14);
        sc->set_child(*rows_box);
        for (const auto& rel : kept) {
            auto rw = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            auto l = text("~/" + rel, "detail-path");
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(m_home + "/" + rel);
            rw->append(*l);
            auto forget = Gtk::make_managed<Gtk::Button>("Forget");
            forget->add_css_class("ghost");
            forget->add_css_class("small");
            forget->signal_clicked().connect([this, rel] {
                Glib::signal_idle().connect_once([this, rel] { on_tidy_keep(rel, false); });
            });
            rw->append(*forget);
            rows_box->append(*rw);
        }
        c->append(*sc);
    }

    if (!r.folders.empty()) {
        auto c = card("Folders that need tidying \u00b7 " + with_commas(r.folders.size()),
                      "Worst first: files arriving, kinds mixed together, a lot unused in a year.");
        for (const auto& f : r.folders)
            line(c, f.rel.empty() ? "~ (the top of home)" : "~/" + f.rel, f.why, with_commas(f.files) + " files", "", "");
    }

    {
        auto c = card("The top of home \u00b7 " + with_commas(r.top.size()) + " folders" +
                      (r.loose.empty() ? "" : " \u00b7 " + with_commas(r.loose.size()) + " loose files"),
                      "What of yours is in each. (Which of these belong at the top -- the skeleton -- is still your call.)");
        for (const auto& t : r.top)
            line(c, "~/" + t.name, t.note.empty() ? scan::human_bytes(t.bytes) : t.note,
                 t.files ? with_commas(t.files) + " files" : "", "", "");
        for (std::size_t i = 0; i < r.loose.size() && i < 12; ++i) line(c, "~/" + r.loose[i], "loose", "", "", "");
        if (r.loose.size() > 12) c->append(*text("... and " + with_commas(r.loose.size() - 12) + " more loose", "dim"));
    }

    if (!r.drift.empty() || !r.notes.empty()) {
        auto c = card("Drift", "Where things land, and what isn't where it says.");
        for (const auto& d : r.drift) { auto l = text("- " + d, "card-sub"); l->set_wrap(true); c->append(*l); }
        for (const auto& n : r.notes) { auto l = text("- " + n, "pf-amber"); l->set_wrap(true); c->append(*l); }
    }    tidy_gate();
}

void Shell::tidy_gate() {  // helper: s016 -- each card's act button: label + may it act
    namespace td = core::tidy;
    const bool busy = m_td_worker.joinable() || door_busy();
    for (const auto& g : m_td_shown.groups) {
        const auto vi = static_cast<std::size_t>(g.verdict);
        if (!m_td_go[vi] || !m_td_gates[vi]) continue;
        std::size_t n = 0;
        std::uint64_t bytes = 0;
        for (const auto& row : g.rows)
            if (m_td_checked.count(row.rel) && m_td_painted.count(row.rel)) { ++n; bytes += row.bytes; }
        const std::string files = with_commas(n) + (n == 1 ? " file" : " files");
        const std::string where = g.verdict == td::Verdict::Dormant ? "~/Archive" : "~/Closet";
        m_td_go[vi]->set_label(n ? (g.verdict == td::Verdict::Undecided ? "Gather " : "Move ") + files + " to " + where
                                 : "Move to " + where);
        m_td_go[vi]->set_sensitive(n > 0 && !m_pf_red && !busy);
        if (auto* all = m_td_all[vi]) {   // s016a3: all / none / some of the offered rows
            std::size_t on = 0;
            for (auto* b : m_td_cbs[vi]) if (b->get_active()) ++on;
            const auto& offered = m_td_cbs[vi];
            m_td_bulk = true;
            all->set_sensitive(!offered.empty() && !busy);
            all->set_inconsistent(on > 0 && on < offered.size());
            all->set_active(!offered.empty() && on == offered.size());
            m_td_bulk = false;
        }
        m_td_gates[vi]->set_text(m_pf_red ? "Preflight has a red check. Tidy waits until it clears -- see Preflight."
                                 : !n ? "Nothing checked."
                                      : scan::human_bytes(bytes) + " checked. One step; Undo takes it back. "
                                        "Hover a row to see where it goes.");
    }
    closet_gate();
}

void Shell::closet_gate() {  // helper: s016b -- the Closet card's buttons: counts, labels, may they act
    namespace td = core::tidy;
    if (!m_tc_trash || !m_tc_junk || !m_tc_gate) return;
    const bool busy = m_td_worker.joinable() || door_busy();
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    std::size_t n = 0, ex = 0;
    std::uint64_t bytes = 0;
    for (const auto& it : m_tc_view.items) {
        if (!m_tc_checked.count(it.rel)) continue;
        ++n; bytes += it.bytes;
        if (td::expired(it, now)) ++ex;
    }
    const auto many = [](std::size_t k) { return with_commas(k) + (k == 1 ? " item" : " items"); };
    m_tc_trash->set_label(ex ? "Move " + many(ex) + " to the Trash" : "Move to the Trash");
    m_tc_trash->set_sensitive(ex > 0 && !m_pf_red && !busy);
    m_tc_junk->set_label(n ? many(n) + " to the Junk Drawer" : "To the Junk Drawer");
    m_tc_junk->set_sensitive(n > 0 && !m_pf_red && !busy);
    m_tc_gate->set_text(m_pf_red ? "Preflight has a red check. Tidy waits until it clears -- see Preflight."
                        : !n ? "Nothing checked. Only expired items can go to the Trash."
                             : scan::human_bytes(bytes) + " checked" +
                                   (ex < n ? " -- " + with_commas(n - ex) + " not expired, so not for the Trash" : "") +
                                   ". One step; Undo takes it back.");
    if (m_tc_all) {
        std::size_t on = 0;
        for (auto& [b, rel] : m_tc_cbs) if (b->get_active()) ++on;
        m_td_bulk = true;
        m_tc_all->set_sensitive(!busy);
        m_tc_all->set_inconsistent(on > 0 && on < m_tc_cbs.size());
        m_tc_all->set_active(!m_tc_cbs.empty() && on == m_tc_cbs.size());
        m_td_bulk = false;
    }
}

void Shell::tidy_done(const std::string& big, const std::string& caption) {  // helper: s016 -- the done state
    m_td_done_big.set_text(big);
    m_td_done_caption.set_text(caption);
    m_td_states.set_visible_child("done");
    paint_activity();
}

void Shell::stop_archive() {  // helper: cancel + join (close, dtor)
    if (m_ar_progress) m_ar_progress->cancel = true;
    if (m_ar_worker.joinable()) m_ar_worker.join();
    m_ar_tick.disconnect();
}

bool Shell::tick_archive() {  // helper: the live counters while looking
    if (!m_ar_progress) return false;
    m_ar_run_big.set_text(with_commas(m_ar_progress->files.load()) + " files");
    if (m_ar_progress->cancel.load()) m_ar_run_caption.set_text("Stopping…");
    return true;
}

void Shell::paint_archive() {  // helper: view switch, cards per top folder / the stored list
    namespace ar = core::archive;
    while (auto* c = m_ar_list.get_first_child()) m_ar_list.remove(*c);

    // The view switch + "untouched for".
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto b_old = Gtk::make_managed<Gtk::ToggleButton>("Old files");
    auto b_in = Gtk::make_managed<Gtk::ToggleButton>("In the archive · " + with_commas(m_ar_stored.size()));
    for (auto* b : {b_old, b_in}) { b->add_css_class("ghost"); b->add_css_class("mode"); }
    b_in->set_group(*b_old);
    b_old->set_active(!m_ar_view_stored);
    b_in->set_active(m_ar_view_stored);
    b_old->signal_toggled().connect([this, b_old] { if (b_old->get_active()) on_archive_view(false); });
    b_in->signal_toggled().connect([this, b_in] { if (b_in->get_active()) on_archive_view(true); });
    top->append(*b_old);
    top->append(*b_in);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    top->append(*spacer);
    if (!m_ar_view_stored) {
        static const std::vector<std::pair<int, const char*>> choices = {
            {6, "6 months"}, {12, "1 year"}, {24, "2 years"}, {36, "3 years"}, {60, "5 years"}};
        top->append(*text("untouched for", "dim", 1.0f));
        std::vector<Glib::ustring> names;
        guint sel = 1;
        for (guint i = 0; i < choices.size(); ++i) {
            names.push_back(choices[i].second);
            if (choices[i].first == m_ar_months) sel = i;
        }
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
        dd->set_selected(sel);
        dd->set_valign(Gtk::Align::CENTER);
        dd->property_selected().signal_changed().connect([this, dd] {
            const guint i = dd->get_selected();
            if (i < choices.size()) Glib::signal_idle().connect_once([this, m = choices[i].first] { on_archive_months(m); });
        });
        top->append(*dd);
    }
    m_ar_list.append(*top);

    const auto month = [](std::int64_t t) {
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tm{};
        ::localtime_r(&tt, &tm);
        char b[16];
        std::strftime(b, sizeof b, "%Y-%m", &tm);
        return std::string(b);
    };
    m_ar_painted = 0;
    if (!m_ar_view_stored) {
        auto& items = m_ar_shown.items;
        std::size_t i = 0;
        const auto key = [](const std::string& rel) {
            const auto p = rel.find('/');
            return p == std::string::npos ? std::string() : rel.substr(0, p);
        };
        while (i < items.size() && m_ar_painted < kArchiveShown) {
            const std::string tf = key(items[i].rel);
            const bool loose = tf.empty();
            std::size_t j = i;
            std::uint64_t gb = 0;
            while (j < items.size() && key(items[j].rel) == tf) gb += items[j++].bytes;
            auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            card->add_css_class("card");
            auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto dot = Gtk::make_managed<Gtk::Box>();
            dot->add_css_class("dot");
            dot->add_css_class("ar-dot");
            dot->set_valign(Gtk::Align::CENTER);
            head->append(*dot);
            auto title = text(loose ? std::string("Loose in your home folder") : "~/" + tf, "card-title");
            title->set_hexpand(true);
            head->append(*title);
            head->append(*text(scan::human_bytes(gb) + " · " + with_commas(j - i) + (j - i == 1 ? " item" : " items"),
                               "card-sub", 1.0f));
            auto all = Gtk::make_managed<Gtk::Button>("Check all");
            all->add_css_class("ghost");
            all->add_css_class("small");
            const std::size_t lo = i, hi = std::min(j, i + (kArchiveShown - m_ar_painted));
            all->signal_clicked().connect([this, lo, hi] {
                for (std::size_t k = lo; k < hi && k < m_ar_shown.items.size(); ++k) m_ar_shown.items[k].checked = true;
                paint_archive();
            });
            head->append(*all);
            card->append(*head);
            for (std::size_t k = i; k < j && m_ar_painted < kArchiveShown; ++k, ++m_ar_painted) {
                const auto& it = items[k];
                auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
                row->add_css_class("sweep-row");
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                cb->set_active(it.checked);
                cb->signal_toggled().connect([this, cb, k] {
                    if (k < m_ar_shown.items.size()) m_ar_shown.items[k].checked = cb->get_active();
                    archive_gate();
                });
                row->append(*cb);
                auto sz = text(scan::human_bytes(it.bytes), "detail-size", 1.0f);
                sz->set_width_chars(9);
                row->append(*sz);
                auto path = text("~/" + it.rel + (it.folder ? "/" : ""), "detail-path");
                path->set_hexpand(true);
                path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
                path->set_tooltip_text(m_home + "/" + it.rel + "\n-> " + m_home + "/" + ar::destination(it.rel, it.year) +
                                       (it.folder ? "\n" + with_commas(it.files) + " files, all untouched since " + month(it.newest) : ""));
                row->append(*path);
                if (it.package) row->append(*text("package · moved whole", "dim", 1.0f));   // s015a
                if (it.folder) row->append(*text(with_commas(it.files) + " files", "dim", 1.0f));
                row->append(*text("last changed " + month(it.newest), "dim", 1.0f));
                card->append(*row);
            }
            m_ar_list.append(*card);
            i = j;
        }
        // nothing past the painted rows may stay checked
        for (std::size_t k = m_ar_painted; k < items.size(); ++k) items[k].checked = false;
        if (items.empty())
            m_ar_list.append(*text("Nothing untouched that long -- your folders are all in use.", "hero-caption", 0.5f));
        std::string foot = "Looked at " + with_commas(m_ar_shown.files) + " files. Left out: hidden folders, caches and "
                           "build output, git repositories, and " ;
        for (std::size_t d = 0; d < ar::dev_roots().size(); ++d)
            foot += (d ? ", ~/" : "~/") + ar::dev_roots()[d];
        foot += " (Dev projects looks after those). Moving into ~/Archive frees no space -- it clears the way. ";
        if (items.size() > m_ar_painted)
            foot += with_commas(items.size() - m_ar_painted) + " more items aren't shown, and stay unchecked. ";
        if (m_ar_shown.cancelled) foot += "Stopped early -- the list is partial. ";
        if (m_ar_shown.errors) foot += with_commas(m_ar_shown.errors) + " things could not be read. ";
        if (m_ar_shown.packages)   // s015a
            foot += with_commas(m_ar_shown.packages) + (m_ar_shown.packages == 1 ? " package" : " packages") +
                    " (Scrivener projects and the like) kept whole: offered only when all of it is untouched, "
                    "and then moved as one. ";
        auto f = text(foot, "dim");
        f->set_wrap(true);
        m_ar_list.append(*f);
    } else {
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        for (std::size_t k = 0; k < m_ar_stored.size() && m_ar_painted < kArchiveShown; ++k, ++m_ar_painted) {
            const auto& s = m_ar_stored[k];
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(s.checked && !s.blocked);
            cb->set_sensitive(!s.blocked);
            cb->signal_toggled().connect([this, cb, k] {
                if (k < m_ar_stored.size()) m_ar_stored[k].checked = cb->get_active();
                archive_gate();
            });
            row->append(*cb);
            auto sz = text(scan::human_bytes(s.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            row->append(*sz);
            auto path = text("~/" + s.back + (s.folder ? "/" : ""), "detail-path");
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text("now at " + m_home + "/" + s.rel);
            row->append(*path);
            row->append(*text(s.blocked ? "something is there now" : "archived " + month(s.when), "dim", 1.0f));
            card->append(*row);
        }
        for (std::size_t k = m_ar_painted; k < m_ar_stored.size(); ++k) m_ar_stored[k].checked = false;
        if (m_ar_stored.empty())
            card->append(*text("Nothing in the archive yet -- or only what Rouster didn't put there.", "dim"));
        m_ar_list.append(*card);
        auto f = text("Read from Rouster's records: everything it moved into ~/Archive that is still there. "
                      "Each goes back to the path it came from; a folder that's gone is made again.", "dim");
        f->set_wrap(true);
        m_ar_list.append(*f);
    }
    archive_gate();
}

void Shell::archive_gate() {  // helper: the act button's label + whether it may act
    std::uint64_t bytes = 0, all = 0;
    std::size_t n = 0;
    if (!m_ar_view_stored) {
        for (const auto& it : m_ar_shown.items) { all += it.bytes; if (it.checked) { bytes += it.bytes; ++n; } }
    } else {
        for (const auto& s : m_ar_stored) { all += s.bytes; if (s.checked && !s.blocked) { bytes += s.bytes; ++n; } }
    }
    const std::string items = with_commas(n) + (n == 1 ? " item" : " items");
    m_ar_big.set_text(scan::human_bytes(bytes));
    m_ar_caption.set_text(std::string(m_ar_view_stored ? "to put back" : "to archive") + ", of " + scan::human_bytes(all) +
                          (m_ar_view_stored ? " in the archive · " : " untouched · ") + items + " checked");
    m_ar_go.set_label(m_ar_view_stored ? (n ? "Put back " + items : "Put back")
                                       : (n ? "Move " + items + " to ~/Archive" : "Move to ~/Archive"));
    const bool busy = m_ar_worker.joinable() || door_busy();
    if (m_pf_red)
        m_ar_gate.set_text("Preflight has a red check. Archive waits until it clears -- see Preflight.");
    else if (!n)
        m_ar_gate.set_text("Nothing checked.");
    else if (m_ar_view_stored)
        m_ar_gate.set_text("Each goes back where it came from, as one step; Undo takes it back again.");
    else
        m_ar_gate.set_text("One step. Undo takes it back; Put back works any time later.");
    if (m_act_ar_run) m_act_ar_run->set_enabled(n > 0 && !m_pf_red && !busy);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rules (s011). Matching reads one folder's top level per watched folder, so it
// runs on the UI thread; the matches shown are the preview.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::rl_save(bool write) {  // helper: rules.json (core::save_own) + the timer
    if (write && !core::save_own(data_dir(), core::rules::kFile, core::rules::to_json(m_rl_book)))
        if (auto lg = log::get(log::Area::Rules)) lg->warn("could not save {}/{}", data_dir(), core::rules::kFile);
    m_rl_timer.disconnect();
    if (m_rl_book.watch) m_rl_timer = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Shell::on_rl_tick), 600);
}

void Shell::paint_rules() {  // helper: the list state -- the rules in order
    namespace rl = core::rules;
    while (auto* c = m_rl_list.get_first_child()) m_rl_list.remove(*c);
    const auto small = [](const char* l) {
        auto b = Gtk::make_managed<Gtk::Button>(l);
        b->add_css_class("ghost");
        b->add_css_class("small");
        b->set_valign(Gtk::Align::CENTER);
        return b;
    };
    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    auto img = Gtk::make_managed<Gtk::Image>();
    img->set_from_icon_name("rouster-hero-rules");
    img->set_pixel_size(88);
    head->append(*img);
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    words->append(*text("Rules", "hero-big"));
    auto cap = text("Each rule watches a folder -- Downloads is the inbox -- and moves what matches. A file goes to the "
                    "first rule that matches it. A rule is turned on only after you've seen its matches.", "hero-caption");
    cap->set_wrap(true);
    words->append(*cap);
    head->append(*words);
    auto run = Gtk::make_managed<Gtk::Button>("Run rules now");
    run->add_css_class("act");
    run->set_valign(Gtk::Align::CENTER);
    std::size_t on = 0;
    for (const auto& r : m_rl_book.rules) on += r.enabled;
    run->set_sensitive(on > 0);
    run->signal_clicked().connect([this] { on_rl_preview(); });
    head->append(*run);
    m_rl_list.append(*head);

    // The controls.
    auto ctl = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    auto add = small("New rule...");
    add->signal_clicked().connect([this] { on_rl_edit(static_cast<std::size_t>(-1)); });
    ctl->append(*add);
    bool missing = false;
    for (const auto& s : rl::starters()) {
        bool have = false;
        for (const auto& r : m_rl_book.rules) have |= r.name == s.name;
        missing |= !have;
    }
    if (missing) {
        auto st = small("Add starter rules (off)");
        st->signal_clicked().connect([this] { on_rl_starters(); });
        ctl->append(*st);
    }
    auto sp = Gtk::make_managed<Gtk::Box>();
    sp->set_hexpand(true);
    ctl->append(*sp);
    ctl->append(*text("Watch while Rouster is open (every 10 minutes)", "dim", 1.0f));
    auto sw = Gtk::make_managed<Gtk::Switch>();
    sw->set_active(m_rl_book.watch);
    sw->set_valign(Gtk::Align::CENTER);
    sw->property_active().signal_changed().connect([this, sw] {
        Glib::signal_idle().connect_once([this, a = sw->get_active()] { on_rl_watch(a); });
    });
    ctl->append(*sw);
    m_rl_list.append(*ctl);
    if (!m_rl_said.empty() || m_rl_deleted) {
        auto said = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        said->append(*text(m_rl_said, "dim"));
        if (m_rl_deleted) {
            auto back = small("Put it back");
            back->signal_clicked().connect([this] { on_rl_undelete(); });
            said->append(*back);
        }
        m_rl_list.append(*said);
    }

    // The rules.
    auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->add_css_class("card");
    const auto n = m_rl_book.rules.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& r = m_rl_book.rules[i];
        const std::string why = rl::problem(r, m_home);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        auto s = Gtk::make_managed<Gtk::Switch>();
        s->set_active(r.enabled);
        s->set_valign(Gtk::Align::CENTER);
        s->property_active().signal_changed().connect([this, s, i] {
            Glib::signal_idle().connect_once([this, i, a = s->get_active()] { on_rl_toggle(i, a); });
        });
        row->append(*s);
        auto w = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
        w->set_hexpand(true);
        w->append(*text(std::to_string(i + 1) + ".  " + r.name, "card-title"));
        auto sum = text(rl::summary(r), "dim");
        sum->set_wrap(true);
        w->append(*sum);
        if (!why.empty()) w->append(*text("Can't run: " + why, "pj-misfit"));
        row->append(*w);
        auto up = small("↑");
        up->set_sensitive(i > 0);
        up->signal_clicked().connect([this, i] { on_rl_move(i, -1); });
        auto dn = small("↓");
        dn->set_sensitive(i + 1 < n);
        dn->signal_clicked().connect([this, i] { on_rl_move(i, +1); });
        auto ed = small("Edit");
        ed->signal_clicked().connect([this, i] { on_rl_edit(i); });
        row->append(*up);
        row->append(*dn);
        row->append(*ed);
        card->append(*row);
    }
    if (!n) card->append(*text("No rules yet. Start from the starters (they come in off) or make your own.", "dim"));
    m_rl_list.append(*card);
    auto foot = text("Rules look only at the top of each watched folder, never at hidden files, partial downloads, or "
                     "anything changed in the last two minutes. Each run is one step in Activity; Undo takes it back.", "dim");
    foot->set_wrap(true);
    m_rl_list.append(*foot);
}

void Shell::paint_rl_matches(Gtk::Box& box, std::vector<core::rules::Match>& ms, bool checks) {  // helper: matches by rule
    while (auto* c = box.get_first_child()) box.remove(*c);
    const auto tilde = [this](const std::string& p) {
        return p.rfind(m_home + "/", 0) == 0 ? "~/" + p.substr(m_home.size() + 1) : p;
    };
    std::size_t painted = 0;
    for (std::size_t ri = 0; ri < std::max<std::size_t>(m_rl_book.rules.size(), 1); ++ri) {
        Gtk::Box* card = nullptr;
        for (std::size_t k = 0; k < ms.size(); ++k) {
            auto& m = ms[k];
            if (checks ? m.rule != ri : ri != 0) continue;
            if (painted >= kProjectShown) { m.checked = false; continue; }   // never unseen
            if (!card) {
                card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
                card->add_css_class("card");
                std::size_t cnt = 0;
                for (const auto& x : ms) cnt += checks ? x.rule == ri : 1;
                const std::string title = checks && ri < m_rl_book.rules.size() ? m_rl_book.rules[ri].name : "Matches now";
                card->append(*text(title + " · " + with_commas(cnt), "card-title"));
            }
            ++painted;
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            if (checks) {
                auto cb = Gtk::make_managed<Gtk::CheckButton>();
                const bool can = m.note.empty();
                cb->set_active(m.checked && can);
                cb->set_sensitive(can);
                cb->signal_toggled().connect([this, cb, k] {
                    if (k < m_rl_matches.size()) m_rl_matches[k].checked = cb->get_active();
                    rl_gate();
                });
                row->append(*cb);
            }
            auto from = text(std::filesystem::path(m.from).filename().string(), "detail-path");
            from->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            from->set_width_chars(22);
            from->set_max_width_chars(32);
            from->set_tooltip_text(m.from);
            row->append(*from);
            row->append(*text("->", "dim"));
            const bool trash = m.to.empty() && m.note.empty();
            auto to = text(!m.note.empty() ? m.note : trash ? std::string("the Trash") : tilde(m.to), m.note.empty() ? "detail-path" : "dim");
            to->set_hexpand(true);
            to->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            to->set_tooltip_text(m.to.empty() ? m.note : m.to);
            row->append(*to);
            auto sz = text(core::scan::human_bytes(m.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            row->append(*sz);
            card->append(*row);
        }
        if (card) box.append(*card);
    }
    if (ms.empty()) box.append(*text(checks ? "Nothing matches any rule that's on right now." : "Nothing matches right now.", "hero-caption", 0.5f));
}

void Shell::rl_gate() {  // helper: the run button's label + whether it may act
    std::size_t n = 0;
    for (const auto& m : m_rl_matches) n += m.checked && m.note.empty();
    m_rl_go.set_label(n ? "Do it for " + with_commas(n) + (n == 1 ? " file" : " files") : "Do it");
    if (m_pf_red && n) m_rl_run_gate.set_text("Preflight has a red check. Rules wait until it clears -- see Preflight.");
    else if (!n) m_rl_run_gate.set_text("Nothing checked.");
    else m_rl_run_gate.set_text("One step; Undo takes it back.");
    m_rl_go.set_sensitive(n > 0 && !m_pf_red && !door_busy());
}

// ─────────────────────────────────────────────────────────────────────────────
// Projects (s010). Reading templates and projects is cheap (a few folders), so
// it happens on the UI thread; every plan is shown step by step before it runs.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::paint_projects() {  // helper: the home state -- templates, projects
    namespace og = core::organize;
    m_pj_templates = og::templates(m_home);
    m_pj_projects = og::projects(m_home, m_pj_templates);
    while (auto* c = m_pj_home.get_first_child()) m_pj_home.remove(*c);
    const auto tilde = [this](const std::string& p) {
        return p.rfind(m_home + "/", 0) == 0 ? "~/" + p.substr(m_home.size() + 1) : p;
    };
    const auto small = [](const char* label) {
        auto b = Gtk::make_managed<Gtk::Button>(label);
        b->add_css_class("ghost");
        b->add_css_class("small");
        b->set_valign(Gtk::Align::CENTER);
        return b;
    };

    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    auto img = Gtk::make_managed<Gtk::Image>();
    img->set_from_icon_name("rouster-hero-projects");
    img->set_pixel_size(88);
    head->append(*img);
    auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    words->set_valign(Gtk::Align::CENTER);
    words->set_hexpand(true);
    words->append(*text("Projects", "hero-big"));
    auto cap = text("A new project is built from a template; files dropped on it are named by its pattern and "
                    "go where their type belongs. Templates are plain folders -- edit them in Files.", "hero-caption");
    cap->set_wrap(true);
    words->append(*cap);
    head->append(*words);
    m_pj_home.append(*head);

    // Templates.
    {
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class("pj-dot");
        dot->set_valign(Gtk::Align::CENTER);
        top->append(*dot);
        auto t = text("Templates · " + with_commas(m_pj_templates.size()), "card-title");
        t->set_hexpand(true);
        top->append(*t);
        std::vector<std::string> skipped;
        if (!og::starters(m_home, skipped).actions.empty()) {
            auto b = small(m_pj_templates.empty() ? "Install starter templates..." : "Add the missing starters...");
            b->signal_clicked().connect([this] { on_pj_starters(); });
            top->append(*b);
        }
        const std::string tdir = m_home + "/" + og::kTemplates;
        if (std::filesystem::is_directory(tdir)) {
            auto b = small("Open templates folder");
            b->signal_clicked().connect([this, tdir] { on_pj_show(tdir); });
            top->append(*b);
        }
        card->append(*top);
        for (std::size_t i = 0; i < m_pj_templates.size(); ++i) {
            const auto& tp = m_pj_templates[i];
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto name = text(tp.name, "detail-path");
            name->set_width_chars(14);
            row->append(*name);
            auto sub = text(tp.problem.empty() ? "~/" + tp.root + "/" + tp.folder + "    " +
                                                     (tp.rules.pattern.empty() ? std::string("names kept") : tp.rules.pattern)
                                               : "can't use it: " + tp.problem, "dim");
            sub->set_hexpand(true);
            sub->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            sub->set_tooltip_text(tp.path);
            row->append(*sub);
            if (tp.problem.empty()) {
                auto b = small("New project...");
                b->signal_clicked().connect([this, i] { on_pj_new(i); });
                row->append(*b);
            }
            card->append(*row);
        }
        if (m_pj_templates.empty())
            card->append(*text("No templates yet. The starters are a Design job, a Book and a gtkmm app; "
                               "they go in ~/" + std::string(og::kTemplates) + ".", "dim"));
        m_pj_home.append(*card);
    }

    // Projects.
    {
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto t = text("Projects · " + with_commas(m_pj_projects.size()), "card-title");
        t->set_hexpand(true);
        top->append(*t);
        auto pick = small("Open a project folder...");
        pick->signal_clicked().connect([this] { on_pj_pick_project(); });
        top->append(*pick);
        card->append(*top);
        for (const auto& p : m_pj_projects) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto name = text(std::filesystem::path(p.path).filename().string(), "detail-path");
            name->set_hexpand(true);
            name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            name->set_tooltip_text(p.path);
            row->append(*name);
            row->append(*text(p.tmpl + " · " + tilde(std::filesystem::path(p.path).parent_path().string()), "dim", 1.0f));
            auto b = small("Open");
            const std::string dir = p.path;
            b->signal_clicked().connect([this, dir] { on_pj_open(dir); });
            row->append(*b);
            card->append(*row);
        }
        if (m_pj_projects.empty())
            card->append(*text("None yet in the templates' folders. A project is any folder holding a " +
                               std::string(og::kMeta) + "; Rouster writes one when it makes a project.", "dim"));
        m_pj_home.append(*card);
    }
}

void Shell::paint_pj_plan(Gtk::Box& box, const core::door::Plan& plan) {  // helper: a plan, step by step (the preview)
    namespace dr = core::door;
    while (auto* c = box.get_first_child()) box.remove(*c);
    auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->add_css_class("card");
    std::size_t shown = 0;
    for (const auto& a : plan.actions) {
        if (shown++ == kProjectShown) {
            card->append(*text("... and " + with_commas(plan.actions.size() - kProjectShown) + " more steps", "dim"));
            break;
        }
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        const char* verb = a.verb == dr::Verb::MakeDir ? "Make folder" : a.verb == dr::Verb::Write ? "Write"
                         : a.verb == dr::Verb::Copy ? "Copy" : a.verb == dr::Verb::Move ? "Move" : dr::verb_name(a.verb);
        auto v = text(verb, "status-chip", 0.5f);
        v->add_css_class("pf-green");
        v->set_width_chars(11);
        row->append(*v);
        const auto tilde = [this](const std::string& p) {
            if (m_pj_has_open && p.rfind(m_pj_open.path + "/", 0) == 0) return p.substr(m_pj_open.path.size() + 1);   // inside the open project
            return p.rfind(m_home + "/", 0) == 0 ? "~/" + p.substr(m_home.size() + 1) : p;
        };
        auto path = text(tilde(a.to) + (a.verb == dr::Verb::MakeDir ? "/" : ""), "detail-path");
        path->set_hexpand(true);
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        std::string tip = a.reason;
        if (a.verb == dr::Verb::Copy || a.verb == dr::Verb::Move) tip = "from " + tilde(a.from) + "\n" + tip;
        if (a.verb == dr::Verb::Write) tip += "\n\n" + a.content;
        path->set_tooltip_text(tip);
        row->append(*path);
        if (a.verb != dr::Verb::MakeDir) {
            auto sz = text(core::scan::human_bytes(a.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            row->append(*sz);
        }
        card->append(*row);
    }
    if (plan.actions.empty()) card->append(*text("Nothing to do.", "dim"));
    box.append(*card);
}

void Shell::paint_pj_project() {  // helper: the open project -- the queue, what's in it
    namespace og = core::organize;
    while (auto* c = m_pj_list.get_first_child()) m_pj_list.remove(*c);
    if (!m_pj_has_open) { pj_gate(); return; }
    const auto& p = m_pj_open;
    const auto in_home = [this](const std::string& s) {
        return s.rfind(m_home + "/", 0) == 0 ? "~/" + s.substr(m_home.size() + 1) : s;
    };
    m_pj_head.set_text(std::filesystem::path(p.path).filename().string());
    std::string routes;
    for (const auto& r : p.rules.routes) routes += (routes.empty() ? "" : ",  ") + r.folder + "/";
    m_pj_head_sub.set_text((p.tmpl.empty() ? std::string("project") : p.tmpl) + "  ·  " + in_home(p.path) + "\n" +
                           (p.rules.pattern.empty() ? std::string("names kept as they are") : p.rules.pattern) +
                           (routes.empty() ? std::string() : "\nroutes to  " + routes));

    // The queue: the preview of a filing.
    if (m_pj_queue.empty()) {
        auto drop = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        drop->add_css_class("pj-drop");
        drop->append(*text("Drop files here -- or Add files...", "card-title", 0.5f));
        drop->append(*text("Each is shown with the name and folder it would get before anything moves.", "dim", 0.5f));
        m_pj_list.append(*drop);
    } else {
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto t = text("To file · " + with_commas(m_pj_queue.size()), "card-title");
        t->set_hexpand(true);
        top->append(*t);
        auto clear = Gtk::make_managed<Gtk::Button>("Clear");
        clear->add_css_class("ghost");
        clear->add_css_class("small");
        clear->signal_clicked().connect([this] { m_pj_queue.clear(); paint_pj_project(); });
        top->append(*clear);
        card->append(*top);
        for (std::size_t k = 0; k < m_pj_queue.size() && k < kProjectShown; ++k) {
            const auto& q = m_pj_queue[k];
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(q.checked && !q.to.empty());
            cb->set_sensitive(!q.to.empty());
            cb->signal_toggled().connect([this, cb, k] {
                if (k < m_pj_queue.size()) m_pj_queue[k].checked = cb->get_active();
                pj_gate();
            });
            row->append(*cb);
            auto from = text(std::filesystem::path(q.from).filename().string(), "detail-path");
            from->set_tooltip_text(q.from);
            from->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            from->set_width_chars(22);
            from->set_max_width_chars(30);
            row->append(*from);
            row->append(*text("->", "dim"));
            auto to = text(q.to.empty() ? q.note : q.to.substr(p.path.size() + 1), q.to.empty() ? "dim" : "detail-path");
            to->set_hexpand(true);
            to->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            to->set_tooltip_text(q.to.empty() ? q.note : q.to);
            row->append(*to);
            card->append(*row);
        }
        for (std::size_t k = kProjectShown; k < m_pj_queue.size(); ++k) m_pj_queue[k].checked = false;   // never unseen
        m_pj_list.append(*card);
    }

    // What's in it.
    const auto entries = og::contents(p, 5000);
    std::size_t misfits = 0;
    for (const auto& e : entries) misfits += !e.fits;
    const std::string& pat = p.rules.pattern;
    const bool datable = pat.find("{yyyymmdd") != std::string::npos;
    auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->add_css_class("card");
    card->append(*text("In the project · " + with_commas(entries.size()) + (entries.size() == 1 ? " file" : " files") +
                       (misfits ? " · " + with_commas(misfits) + " don't fit the pattern" : std::string()), "card-title"));
    std::size_t shown = 0;
    for (const auto& e : entries) {
        if (shown++ == kProjectShown) {
            card->append(*text("... and " + with_commas(entries.size() - kProjectShown) + " more", "dim"));
            break;
        }
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        auto sz = text(core::scan::human_bytes(e.bytes), "detail-size", 1.0f);
        sz->set_width_chars(9);
        row->append(*sz);
        auto path = text(e.rel, e.versioned ? "dim" : "detail-path");
        path->set_hexpand(true);
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        path->set_tooltip_text(p.path + "/" + e.rel);
        row->append(*path);
        if (!e.fits) {
            auto chip = text("doesn't fit", "pj-misfit", 0.5f);
            chip->set_valign(Gtk::Align::CENTER);
            chip->set_tooltip_text("Flagged, not moved. Drop it on the project to name it by the pattern.");
            row->append(*chip);
        } else if (datable && !e.versioned) {
            auto b = Gtk::make_managed<Gtk::Button>("New version");
            b->add_css_class("ghost");
            b->add_css_class("small");
            const std::string rel = e.rel;
            b->signal_clicked().connect([this, rel] { on_pj_version(rel); });
            row->append(*b);
        }
        card->append(*row);
    }
    if (entries.empty()) card->append(*text("Empty so far.", "dim"));
    m_pj_list.append(*card);
    pj_gate();
}

void Shell::pj_gate() {  // helper: the file button's label + whether it may act
    std::size_t n = 0;
    for (const auto& q : m_pj_queue) n += q.checked && !q.to.empty();
    m_pj_go.set_label(n ? "File " + with_commas(n) + (n == 1 ? " file" : " files") : "File");
    if (m_pf_red && n)
        m_pj_gate.set_text("Preflight has a red check. Filing waits until it clears -- see Preflight.");
    else if (!n)
        m_pj_gate.set_text(m_pj_queue.empty() ? "Drop files on the page to see where they'd go." : "Nothing checked.");
    else
        m_pj_gate.set_text("Each is moved (never copied) under its new name. One step; Undo takes it back.");
    m_pj_go.set_sensitive(n > 0 && !m_pf_red && !door_busy());
}

void Shell::pj_done(const core::door::Result& r, const std::string& big, const std::string& what, const std::string& back) {  // helper: the done state
    m_pj_back = back;
    if (r.ok) {
        m_pj_done_big.set_text(big);
        m_pj_done_caption.set_text(what + " " + std::to_string(r.done) + (r.done == 1 ? " step" : " steps") +
                                   "; Undo takes them all back.");
    } else if (r.done == 0) {
        m_pj_done_big.set_text("Nothing changed");
        m_pj_done_caption.set_text(r.messages.empty() ? std::string("The door refused the plan.") : r.messages.front());
    } else {
        m_pj_done_big.set_text(std::to_string(r.done) + " of " + std::to_string(r.done + r.failed));
        m_pj_done_caption.set_text("steps done; " + std::to_string(r.failed) + " could not: " +
                                   (r.messages.empty() ? std::string("see Activity") : r.messages.front()) +
                                   ". Undo puts back what changed.");
    }
    m_pj_done_back.set_label(back == "project" && m_pj_has_open ? "Back to the project" : "Back");
    m_pj_states.set_visible_child("done");
    paint_activity();
}


// ─────────────────────────────────────────────────────────────────────────────
// Profile backup (s012a). The worker runs core::profile::find (read-only:
// files, dconf dump, dnf -C, flatpak list); the cards are the preview; checks
// live in m_pr_shown. Rows beyond kProfileShown in a group are never checked.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::stop_profile() {  // helper: cancel + join (close, dtor)
    m_pr_cancel = true;
    if (m_pr_worker.joinable()) m_pr_worker.join();
}

void Shell::paint_profile() {  // helper: cards per group, rows with checks, what wasn't gathered
    namespace pr = core::profile;
    while (auto* c = m_pr_list.get_first_child()) m_pr_list.remove(*c);
    auto& items = m_pr_shown.items;
    for (int gi = 0; gi < pr::kGroupCount; ++gi) {
        const auto g = static_cast<pr::Group>(gi);
        std::uint64_t gbytes = 0;
        std::size_t n = 0;
        for (const auto& it : items) if (it.group == g) { gbytes += it.bytes; ++n; }
        if (!n) continue;
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class("pr-dot");
        dot->set_valign(Gtk::Align::CENTER);
        head->append(*dot);
        auto title = text(pr::group_name(g), "card-title");
        title->set_hexpand(true);
        head->append(*title);
        head->append(*text(scan::human_bytes(gbytes) + " · " + with_commas(n) + (n == 1 ? " item" : " items"), "card-sub", 1.0f));
        card->append(*head);
        auto why = text(pr::group_why(g), "dim");
        why->set_wrap(true);
        why->set_margin_bottom(4);
        card->append(*why);
        std::size_t shown = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].group != g) continue;
            if (shown++ == kProfileShown) {
                card->append(*text("... and " + with_commas(n - kProfileShown) + " more, left out (not shown, not checked)", "dim"));
                for (std::size_t k = i; k < items.size(); ++k) if (items[k].group == g) items[k].checked = false;
                break;
            }
            const auto& it = items[i];
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(it.checked);
            cb->signal_toggled().connect([this, cb, i] {
                if (i < m_pr_shown.items.size()) m_pr_shown.items[i].checked = cb->get_active();
                profile_gate();
            });
            auto sz = text(scan::human_bytes(it.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            auto path = text(it.label, "detail-path");
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text("-> " + it.dest + (it.read ? "\nread and written (it sits in a protected folder)" : ""));
            row->append(*cb);
            row->append(*sz);
            row->append(*path);
            if (!it.note.empty()) row->append(*text(it.note, "dim", 1.0f));
            card->append(*row);
            if (it.made) {
                // s012c: a made row (dconf dump, a list) opens in place, whole -- a
                // tooltip vanished when the mouse moved and was cut at 1500 chars.
                // The label is built on first open (a dconf dump can be 100 KB).
                auto ex = Gtk::make_managed<Gtk::Expander>("What it holds");
                ex->set_margin_start(40);
                ex->property_expanded().signal_changed().connect([this, ex, i] {
                    if (!ex->get_expanded() || ex->get_child() || i >= m_pr_shown.items.size()) return;
                    auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
                    sc->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
                    sc->set_max_content_height(360);
                    sc->set_propagate_natural_height(true);
                    auto d = text(m_pr_shown.items[i].content, "pr-diff");
                    d->set_selectable(true);
                    d->set_wrap(false);
                    d->set_valign(Gtk::Align::START);
                    sc->set_child(*d);
                    ex->set_child(*sc);
                });
                card->append(*ex);
            }
        }
        m_pr_list.append(*card);
    }
    if (items.empty()) m_pr_list.append(*text("Nothing found to back up.", "hero-caption", 0.5f));

    // s012c: secrets -- opt-in, encrypted, a passphrase typed now and never kept.
    m_pr_sec_on = nullptr;
    m_pr_pass1 = m_pr_pass2 = nullptr;
    m_pr_sec_state = nullptr;
    {
        const auto& sf = m_pr_shown.secrets;
        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        card->add_css_class("card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class("pr-secrets-dot");
        dot->set_valign(Gtk::Align::CENTER);
        head->append(*dot);
        auto t = text("Secrets -- ssh keys, gpg, keyrings", "card-title");
        t->set_hexpand(true);
        head->append(*t);
        head->append(*text(sf.files.empty() ? std::string("none found")
                                            : scan::human_bytes(sf.bytes) + " · " + with_commas(sf.files.size()) +
                                                  (sf.files.size() == 1 ? " file" : " files"),
                           "card-sub", 1.0f));
        card->append(*head);
        auto why = text("Left out unless you choose. Chosen, they go in as ONE file, secrets.tar.gpg, encrypted with gpg "
                        "(AES256) under a passphrase you type here. Rouster never stores it -- lose it and they can't be "
                        "opened, by you or anyone.", "dim");
        why->set_wrap(true);
        card->append(*why);
        if (!sf.files.empty()) {
            static const char* where[3] = {"~/.ssh", "~/.gnupg", "~/.local/share/keyrings"};
            std::string list;
            for (int k = 0; k < 3; ++k)
                if (sf.per_root[k])
                    list += (list.empty() ? "" : " · ") + std::string(where[k]) + " " + with_commas(sf.per_root[k]);
            card->append(*text(list, "detail-path"));
            for (const auto& n : sf.notes) card->append(*text(n, "dim"));
            auto on = Gtk::make_managed<Gtk::CheckButton>("Include them, encrypted");
            on->set_active(false);
            m_pr_sec_on = on;
            card->append(*on);
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            box->set_margin_start(28);
            auto p1 = Gtk::make_managed<Gtk::PasswordEntry>();
            auto p2 = Gtk::make_managed<Gtk::PasswordEntry>();
            p1->set_show_peek_icon(true);
            p2->set_show_peek_icon(true);
            p1->set_property("placeholder-text", Glib::ustring("Passphrase"));
            p2->set_property("placeholder-text", Glib::ustring("The same again"));
            p1->set_hexpand(true);
            p2->set_hexpand(true);
            box->append(*p1);
            box->append(*p2);
            box->set_sensitive(false);
            m_pr_pass1 = p1;
            m_pr_pass2 = p2;
            card->append(*box);
            auto state = text("", "dim");
            state->set_margin_start(28);
            state->set_wrap(true);
            m_pr_sec_state = state;
            card->append(*state);
            on->signal_toggled().connect([this, box, on] {
                box->set_sensitive(on->get_active());
                if (on->get_active() && m_pr_pass1) m_pr_pass1->grab_focus();
                profile_gate();
            });
            p1->signal_changed().connect([this] { profile_gate(); });
            p2->signal_changed().connect([this] { profile_gate(); });
        }
        m_pr_list.append(*card);
    }
    if (!m_pr_shown.notes.empty()) {
        auto miss = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        miss->add_css_class("detail");
        miss->append(*text("Not in this backup", "card-title"));
        std::string body;
        for (const auto& n : m_pr_shown.notes) body += (body.empty() ? "" : "\n") + n;
        auto mb = text(body, "dim");
        mb->set_wrap(true);
        miss->append(*mb);
        m_pr_list.append(*miss);
    }
    profile_gate();
}

void Shell::profile_gate() {  // helper: the act button's label + whether it may act
    std::uint64_t bytes = 0, all = 0;
    std::size_t n = 0;
    for (const auto& it : m_pr_shown.items) {
        all += it.bytes;
        if (it.checked) { bytes += it.bytes; ++n; }
    }
    m_pr_big.set_text(scan::human_bytes(bytes));
    m_pr_caption.set_text("chosen, of " + scan::human_bytes(all) + " · " + with_commas(n) + " of " +
                          with_commas(m_pr_shown.items.size()) + " items · from " + m_pr_shown.host);
    // s012c: secrets, if chosen, need a passphrase typed twice.
    const bool sec = m_pr_sec_on && m_pr_sec_on->get_active() && !m_pr_shown.secrets.files.empty();
    std::string problem;
    if (sec && m_pr_pass1 && m_pr_pass2) {
        std::string a = m_pr_pass1->get_text(), b = m_pr_pass2->get_text();
        problem = core::secrets::passphrase_problem(a, b);
        core::secrets::wipe(a);
        core::secrets::wipe(b);
    }
    if (m_pr_sec_state)
        m_pr_sec_state->set_text(!sec ? std::string()
                                 : problem.empty() ? "The two match. Keep it somewhere safe -- Rouster won't."
                                                   : problem);
    m_pr_go.set_label(n ? "Back up " + scan::human_bytes(bytes) + (sec ? " + secrets" : "") : "Back up");
    const bool busy = m_pr_worker.joinable() || door_busy() || m_pr_packing;
    m_pr_gate.set_text(!n ? "Nothing checked."
                       : sec && !problem.empty() ? "Secrets: " + problem
                       : std::string("One new folder in ~/Archive/Profile; nothing else changes. Undo sends it to the Trash.") +
                             (sec ? " The secrets go in encrypted." : ""));
    if (m_act_pr_run) m_act_pr_run->set_enabled(n > 0 && !busy && problem.empty());
}

void Shell::paint_profile_start() {  // helper: the earlier backups, each with Restore...
    while (auto* c = m_pr_backups.get_first_child()) m_pr_backups.remove(*c);
    const auto bs = core::profile::bundles(m_home);
    if (bs.empty()) return;
    auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->add_css_class("card");
    card->append(*text(bs.size() == 1 ? "Your backup" : "Your backups", "card-title"));
    for (const auto& n : bs) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        auto name = text("~/" + std::string(core::profile::kRoot) + "/" + n, "detail-path");
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        row->append(*name);
        auto b = Gtk::make_managed<Gtk::Button>("Restore…");
        b->add_css_class("ghost");
        b->signal_clicked().connect([this, n] { on_profile_look_back(n); });
        row->append(*b);
        card->append(*row);
    }
    m_pr_backups.append(*card);
}

void Shell::paint_profile_restore() {  // helper: the restore preview -- files by state, diffs, commands
    namespace pr = core::profile;
    while (auto* c = m_pr_rs_list.get_first_child()) m_pr_rs_list.remove(*c);
    auto& files = m_pr_rs.files;
    std::size_t same = 0, prot = 0, blocked = 0;
    for (const auto& b : files) {
        if (b.state == pr::State::Same) ++same;
        if (b.state == pr::State::Protected) ++prot;
        if (b.state == pr::State::Blocked) ++blocked;
    }
    auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    card->add_css_class("card");
    card->append(*text("Files", "card-title"));
    auto why = text("Missing ones are checked. One that differs is not: read the difference first. Restoring it "
                    "sends the one there now to the Trash and puts the backup's in its place -- Undo swaps them back.", "dim");
    why->set_wrap(true);
    why->set_margin_bottom(4);
    card->append(*why);
    std::size_t shown = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& b = files[i];
        if (b.state != pr::State::Missing && b.state != pr::State::Differs) continue;
        if (shown++ == kProfileShown) {
            card->append(*text("... more not shown (and not checked)", "dim"));
            for (std::size_t k = i; k < files.size(); ++k) files[k].checked = false;
            break;
        }
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        auto cb = Gtk::make_managed<Gtk::CheckButton>();
        cb->set_active(b.checked);
        cb->signal_toggled().connect([this, cb, i] {
            if (i < m_pr_rs.files.size()) m_pr_rs.files[i].checked = cb->get_active();
            profile_restore_gate();
        });
        auto chip = text(b.state == pr::State::Missing ? "missing" : "differs", "status-chip", 0.5f);
        chip->add_css_class(b.state == pr::State::Missing ? "pf-green" : "pf-amber");
        chip->set_width_chars(9);
        auto path = text("~/" + b.rel, "detail-path");
        path->set_hexpand(true);
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        auto sz = text(scan::human_bytes(b.bytes), "detail-size", 1.0f);
        sz->set_width_chars(9);
        row->append(*cb);
        row->append(*chip);
        row->append(*path);
        row->append(*sz);
        card->append(*row);
        if (b.state == pr::State::Differs) {
            auto ex = Gtk::make_managed<Gtk::Expander>("The difference");
            ex->set_margin_start(40);
            auto d = text(b.diff, "pr-diff");
            d->set_selectable(true);
            d->set_wrap(false);
            ex->set_child(*d);
            card->append(*ex);
        }
    }
    if (!shown) card->append(*text("Nothing to put back -- every file is as the backup has it.", "dim"));
    std::string foot;
    if (same) foot += with_commas(same) + (same == 1 ? " file is" : " files are") + " the same as now. ";
    if (prot) foot += with_commas(prot) + " in a protected folder (never written -- see below). ";
    if (blocked) foot += with_commas(blocked) + " blocked (a folder or link is where the file would go). ";
    if (!foot.empty()) card->append(*text(foot, "dim"));
    m_pr_rs_list.append(*card);

    // A line to run yourself, with its Copy (s012c: no hyphens, word breaks first).
    const auto add_line = [this](Gtk::Box& into, const std::string& what_s, const std::string& line_s) {
        auto what = text(what_s, nullptr);
        what->set_margin_top(6);
        what->set_wrap(true);
        into.append(*what);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto line = text(line_s, "pr-diff");
        line->set_selectable(true);
        line->set_wrap(true);
        line->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
        Pango::AttrList attrs;
        auto nohy = Pango::Attribute::create_attr_insert_hyphens(false);
        attrs.insert(nohy);
        line->set_attributes(attrs);
        line->set_hexpand(true);
        row->append(*line);
        auto copy_btn = Gtk::make_managed<Gtk::Button>("Copy");
        copy_btn->add_css_class("ghost");
        copy_btn->set_valign(Gtk::Align::START);
        copy_btn->signal_clicked().connect([this, l = line_s] { get_clipboard()->set_text(l); });
        row->append(*copy_btn);
        into.append(*row);
    };

    // s012d: GNOME settings -- key by key; unchecked (it changes the live desktop).
    if (m_pr_rs.settings.there && (m_pr_rs.settings.differs || !m_pr_rs.settings.error.empty())) {
        const auto& st = m_pr_rs.settings;
        auto gc = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        gc->add_css_class("card");
        gc->append(*text("GNOME settings", "card-title"));
        if (!st.error.empty()) {
            auto e = text("Can't compare: " + st.error + ".", "dim");
            e->set_wrap(true);
            gc->append(*e);
        } else {
            auto w = text("Not checked: this changes your desktop as soon as it runs. Your settings now are saved first; "
                          "Undo puts them back exactly. Keys you set since the backup that it doesn't name stay as they are.",
                          "dim");
            w->set_wrap(true);
            gc->append(*w);
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(st.checked);
            cb->signal_toggled().connect([this, cb] {
                m_pr_rs.settings.checked = cb->get_active();
                profile_restore_gate();
            });
            auto chip = text("differs", "status-chip", 0.5f);
            chip->add_css_class("pf-amber");
            chip->set_width_chars(9);
            auto what = text(with_commas(st.changes) + (st.changes == 1 ? " key" : " keys") + " would change" +
                                 (st.kept ? " · " + with_commas(st.kept) + " set since, kept" : std::string()),
                             "detail-path");
            what->set_hexpand(true);
            row->append(*cb);
            row->append(*chip);
            row->append(*what);
            gc->append(*row);
            auto ex = Gtk::make_managed<Gtk::Expander>("What would change");
            ex->set_margin_start(40);
            ex->property_expanded().signal_changed().connect([this, ex] {
                if (!ex->get_expanded() || ex->get_child()) return;
                auto sc = Gtk::make_managed<Gtk::ScrolledWindow>();
                sc->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
                sc->set_max_content_height(360);
                sc->set_propagate_natural_height(true);
                auto d = text(m_pr_rs.settings.what, "pr-diff");
                d->set_selectable(true);
                d->set_wrap(false);
                d->set_valign(Gtk::Align::START);
                sc->set_child(*d);
                ex->set_child(*sc);
            });
            gc->append(*ex);
        }
        m_pr_rs_list.append(*gc);
    }

    // s012c: the encrypted secrets -- opened in memory with the passphrase;
    // never written by Rouster (they belong in never-touch folders).
    m_pr_rs_pass = nullptr;
    if (m_pr_rs.secrets) {
        namespace sc = core::secrets;
        auto sc_card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        sc_card->add_css_class("card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto t = text("Secrets -- ssh keys, gpg, keyrings", "card-title");
        t->set_hexpand(true);
        head->append(*t);
        head->append(*text(scan::human_bytes(m_pr_rs.secrets) + " encrypted", "card-sub", 1.0f));
        sc_card->append(*head);
        if (m_pr_sec.rows.empty()) {
            auto w = text("This backup holds them in secrets.tar.gpg. Type the passphrase it was made with to compare them "
                          "with what you have now -- opened in memory only; nothing is written.", "dim");
            w->set_wrap(true);
            sc_card->append(*w);
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
            auto pe = Gtk::make_managed<Gtk::PasswordEntry>();
            pe->set_show_peek_icon(true);
            pe->set_property("placeholder-text", Glib::ustring("Passphrase"));
            pe->set_hexpand(true);
            pe->set_sensitive(!m_pr_worker.joinable());
            m_pr_rs_pass = pe;
            auto open_btn = Gtk::make_managed<Gtk::Button>("Open");
            open_btn->add_css_class("ghost");
            open_btn->signal_clicked().connect([this] { on_profile_open_secrets(); });
            // Enter opens too. gtkmm 4.10 wraps no signal_activate() on PasswordEntry; the GTK signal is there.
            g_signal_connect(pe->gobj(), "activate",
                             G_CALLBACK(+[](GtkPasswordEntry*, gpointer self) { static_cast<Shell*>(self)->on_profile_open_secrets(); }),
                             this);
            row->append(*pe);
            row->append(*open_btn);
            sc_card->append(*row);
            if (!m_pr_sec.error.empty()) {
                auto e = text(m_pr_sec.error, "pf-red");
                e->set_wrap(true);
                sc_card->append(*e);
            }
        } else {
            std::size_t same = 0;
            for (const auto& r : m_pr_sec.rows) {
                if (r.state == sc::State::Same) { ++same; continue; }
                auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
                row->add_css_class("sweep-row");
                auto chip = text(sc::state_name(r.state), "status-chip", 0.5f);
                chip->add_css_class(r.state == sc::State::Missing ? "pf-green" : r.state == sc::State::Differs ? "pf-amber" : "pf-red");
                chip->set_width_chars(9);
                auto path = text("~/" + r.rel, "detail-path");
                path->set_hexpand(true);
                path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
                auto sz = text(scan::human_bytes(r.bytes), "detail-size", 1.0f);
                sz->set_width_chars(9);
                row->append(*chip);
                row->append(*path);
                row->append(*sz);
                sc_card->append(*row);
            }
            sc_card->append(*text(same == m_pr_sec.rows.size()
                                      ? "All " + with_commas(same) + " are the same as now -- nothing to put back."
                                      : with_commas(same) + (same == 1 ? " is" : " are") + " the same as now.",
                                  "dim"));
            if (!m_pr_sec.commands.empty()) {
                auto w = text("Rouster never writes into ~/.ssh, ~/.gnupg or the keyrings. These lines do it; gpg asks for "
                              "the passphrase again.", "dim");
                w->set_wrap(true);
                sc_card->append(*w);
                for (const auto& c : m_pr_sec.commands) add_line(*sc_card, c.what, c.line);
            }
        }
        m_pr_rs_list.append(*sc_card);
    }

    if (!m_pr_rs.commands.empty() || !m_pr_rs.notes.empty()) {
        auto cc = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        cc->add_css_class("card");
        cc->append(*text("To run yourself", "card-title"));
        auto w = text("Rouster doesn't install packages or change settings it can't undo. Each line is ready to paste "
                      "in a terminal.", "dim");
        w->set_wrap(true);
        cc->append(*w);
        for (const auto& c : m_pr_rs.commands) {
            add_line(*cc, c.what, c.line);
        }
        for (const auto& n : m_pr_rs.notes) cc->append(*text(n, "dim"));
        m_pr_rs_list.append(*cc);
    }
    profile_restore_gate();
}

void Shell::profile_restore_gate() {  // helper: the restore button's label + whether it may act
    std::size_t n = 0, swaps = 0;
    for (const auto& b : m_pr_rs.files)
        if (b.checked && (b.state == core::profile::State::Missing || b.state == core::profile::State::Differs)) {
            ++n;
            if (b.state == core::profile::State::Differs) ++swaps;
        }
    const bool set = m_pr_rs.settings.differs && m_pr_rs.settings.checked;   // s012d
    m_pr_rs_go.set_label(n && set ? "Restore " + with_commas(n) + (n == 1 ? " file" : " files") + " + settings"
                         : n      ? "Restore " + with_commas(n) + (n == 1 ? " file" : " files")
                         : set    ? std::string("Restore GNOME settings")
                                  : std::string("Restore"));
    const bool busy = m_pr_worker.joinable() || door_busy();
    if (m_pf_red)
        m_pr_rs_gate.set_text("Preflight has a red check. Restore waits until it clears -- see Preflight.");
    else if (!n && !set)
        m_pr_rs_gate.set_text("Nothing checked.");
    else
        m_pr_rs_gate.set_text((swaps ? with_commas(swaps) + " replaced (the one there goes to the Trash)."
                                     : n ? std::string("Nothing there is replaced.") : std::string()) +
                              (set ? std::string(n ? " " : "") + "Your settings now are saved first." : std::string()) +
                              " One step; Undo puts it all back.");
    m_pr_rs_go.set_sensitive((n > 0 || set) && !busy && !m_pf_red);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dev projects (s007). The worker runs core::dev::find (read-only, git with
// optional locks off); the cards are the preview; checks live in m_dv_shown.
// ─────────────────────────────────────────────────────────────────────────────
void Shell::stop_dev() {  // helper: cancel + join (close, dtor)
    if (m_dv_progress) m_dv_progress->cancel = true;
    if (m_dv_worker.joinable()) m_dv_worker.join();
    m_dv_tick.disconnect();
    if (m_dv_act_worker.joinable()) m_dv_act_worker.join();   // a clone in flight finishes; the door stays whole
}

bool Shell::door_busy() const {  // helper: a retire / restore / duplicates run owns the door right now
    return m_dv_acting || m_du_acting || m_pf_acting || m_sy_acting;
}

bool Shell::tick_dev() {  // helper: the live counters while looking
    if (!m_dv_progress) return false;
    const auto n = m_dv_progress->files.load();
    m_dv_run_big.set_text(with_commas(n) + (n == 1 ? " project" : " projects"));
    m_dv_run_caption.set_text(m_dv_progress->cancel.load()
        ? "Stopping…"
        : scan::human_bytes(m_dv_progress->bytes.load()) + " of build output · " +
          with_commas(m_dv_progress->dirs.load()) + " folders looked in");
    return true;
}

void Shell::paint_dev() {  // helper: one card per project: verdict, checks, lint, build rows
    namespace dv = core::dev;
    while (auto* c = m_dv_list.get_first_child()) m_dv_list.remove(*c);
    auto& projects = m_dv_shown.projects;

    for (std::size_t pi = 0; pi < projects.size(); ++pi) {
        const auto& p = projects[pi];
        const auto st = p.repo.status;
        const char* tone = st == dv::Status::Backed ? "pf-green" : st == dv::Status::Retired ? "dim"
                         : (st == dv::Status::NoRemote || st == dv::Status::NotRepo) ? "pf-red" : "pf-amber";
        const char* dotc = st == dv::Status::Backed ? "dv-backed" : st == dv::Status::Retired ? "dv-retired"
                         : (st == dv::Status::NoRemote || st == dv::Status::NotRepo) ? "dv-unsafe" : "dv-notclean";

        auto card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");
        card->add_css_class("dev-card");
        auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto dot = Gtk::make_managed<Gtk::Box>();
        dot->add_css_class("dot");
        dot->add_css_class(dotc);
        dot->set_valign(Gtk::Align::CENTER);
        head->append(*dot);
        head->append(*text(p.name, "card-title"));
        auto where = text("~/" + p.rel + (p.kind == dv::Kind::Workspace ? "  · workspace" : "  · repo"), "card-sub");
        where->set_hexpand(true);
        where->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        head->append(*where);
        auto chip = text(dv::status_name(st), "status-chip");
        chip->add_css_class(tone);
        chip->set_valign(Gtk::Align::CENTER);
        head->append(*chip);
        // Retire (a backed-up workspace) / Restore (a retired one): each opens a preview first.
        if (p.kind == dv::Kind::Workspace && (st == dv::Status::Backed || st == dv::Status::Retired)) {
            auto b = Gtk::make_managed<Gtk::Button>(st == dv::Status::Retired ? "Restore…" : "Retire…");
            b->add_css_class("ghost");
            b->add_css_class("small");
            b->set_valign(Gtk::Align::CENTER);
            b->set_tooltip_text(st == dv::Status::Retired
                ? "Clone it back from " + p.repo.host + " -- you see the plan first"
                : "Send the working copy and old backups to the Trash, leave RESTORE.txt -- you see the plan first");
            const std::string rel = p.rel;
            const bool retired = st == dv::Status::Retired;
            b->signal_clicked().connect([this, rel, retired] { retired ? on_dev_restore(rel) : on_dev_retire(rel); });
            head->append(*b);
        }
        card->append(*head);

        // The verdict, in one line, then whatever failed.
        std::string verdict;
        const std::string sha = p.repo.head.empty() ? std::string("no commits") : p.repo.head.substr(0, 7);
        switch (st) {
            case dv::Status::Backed:
                verdict = "On " + p.repo.host + " (" + p.repo.remote + ") at " + sha +
                          (p.repo.branch.empty() ? "" : " on " + p.repo.branch) +
                          " -- committed, pushed, no local-only branches, no stashes, tags pushed; asked live.";
                break;
            case dv::Status::NotClean:
                verdict = "Not everything here is on " + p.repo.remote + " (" + p.repo.host + "):";
                break;
            case dv::Status::Unreachable:
                verdict = "Couldn't ask " + p.repo.remote + " (" + p.repo.host + "): " + p.repo.error +
                          ". Unknown, so treated as not clean.";
                break;
            case dv::Status::NoRemote:
            case dv::Status::NotRepo:
                verdict = "Not backed up: " + p.repo.error + ". Rouster leaves it alone.";
                break;
            case dv::Status::Retired:
                verdict = "Retired " + p.restore.retired + " -- the working copy lives on " + p.repo.host + " at " + sha +
                          (p.repo.branch.empty() ? "" : " (" + p.repo.branch + ")") + ". RESTORE.txt says how to get it back.";
                break;
        }
        auto v = text(verdict, st == dv::Status::Backed ? "dim" : tone);
        v->set_wrap(true);
        card->append(*v);
        if (st == dv::Status::NotClean)
            for (const auto& c : p.repo.checks) {
                if (c.ok) continue;
                auto l = text("•  " + c.name + ": " + c.detail, "dim");
                l->set_wrap(true);
                l->set_margin_start(12);
                card->append(*l);
            }
        if (!p.repo.other_remotes.empty()) {
            std::string o;
            for (const auto& r : p.repo.other_remotes) o += (o.empty() ? "" : ", ") + r;
            card->append(*text("Also has " + o + " -- checked against " + p.repo.remote + " only.", "dim"));
        }
        if (!p.repo.ignored.empty()) {
            std::string ig;
            for (std::size_t i = 0; i < p.repo.ignored.size() && i < 6; ++i) ig += (i ? ", " : "") + p.repo.ignored[i];
            if (p.repo.ignored.size() > 6) ig += " and " + std::to_string(p.repo.ignored.size() - 6) + " more";
            auto l = text("Ignored, but here: " + ig, "dim");
            l->set_wrap(true);
            l->set_tooltip_text("Git ignores these, so they are not on the remote. Retire would lose them -- decide first.");
            card->append(*l);
        }
        // Template drift (workspaces).
        for (const auto& d : p.drift) {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto tag = text(dv::tier_name(d.tier), "drift-tag");
            tag->add_css_class(d.tier == dv::Tier::Required ? "pf-red" : d.tier == dv::Tier::Note ? "dim" : "pf-amber");
            row->append(*tag);
            auto w = text(d.what, "dim");
            w->set_wrap(true);
            w->set_hexpand(true);
            row->append(*w);
            if (!d.name.empty() && (d.tier == dv::Tier::Unknown || d.promoted)) {
                auto k = Gtk::make_managed<Gtk::Button>(d.promoted ? "Forget" : "Known");
                k->add_css_class("ghost");
                k->add_css_class("small");
                k->set_valign(Gtk::Align::CENTER);
                k->set_tooltip_text(d.promoted ? "Not known-optional after all: report " + d.name + " again"
                                               : "Known-optional from now on, in every workspace: stop reporting " + d.name);
                const std::string rel = p.rel, name = d.name;
                const bool add = !d.promoted;
                k->signal_clicked().connect([this, rel, name, add] { on_dev_known(rel, name, add); });
                row->append(*k);
            }
            card->append(*row);
        }
        // Build output: a check each (shown, unchecked and insensitive when not offered).
        for (std::size_t bi = 0; bi < p.builds.size(); ++bi) {
            const auto& b = p.builds[bi];
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            row->add_css_class("sweep-row");
            auto cb = Gtk::make_managed<Gtk::CheckButton>();
            cb->set_active(b.offered && b.checked);
            cb->set_sensitive(b.offered);
            cb->signal_toggled().connect([this, cb, pi, bi] {
                if (pi < m_dv_shown.projects.size() && bi < m_dv_shown.projects[pi].builds.size())
                    m_dv_shown.projects[pi].builds[bi].checked = cb->get_active();
                dev_gate();
            });
            auto sz = text(scan::human_bytes(b.bytes), "detail-size", 1.0f);
            sz->set_width_chars(9);
            auto path = text("~/" + b.rel, "detail-path");
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text(m_home + "/" + b.rel + "  —  " + with_commas(b.files) +
                                   (b.files == 1 ? " file" : " files") + " of build output");
            row->append(*cb);
            row->append(*sz);
            row->append(*path);
            if (!b.note.empty()) row->append(*text(b.note, "dim", 1.0f));
            card->append(*row);
        }
        m_dv_list.append(*card);
    }
    if (projects.empty()) {
        std::string roots;
        for (const auto& r : core::dev::roots()) roots += (roots.empty() ? "~/" : ", ~/") + r;
        m_dv_list.append(*text(m_dv_shown.roots.empty() ? "No dev folder here -- Rouster looks in " + roots + "."
                                                        : "No workspaces or git repos found.", "hero-caption", 0.5f));
    }
    std::string foot;
    if (m_dv_shown.cancelled) foot += "Stopped early -- the list is partial. ";
    if (m_dv_shown.errors) foot += with_commas(m_dv_shown.errors) + " folders could not be read. ";
    if (!foot.empty()) m_dv_list.append(*text(foot, "dim"));
    dev_gate();
}

void Shell::dev_gate() {  // helper: the light-clean button's label + whether it may act
    namespace dv = core::dev;
    std::uint64_t bytes = 0, all = 0;
    std::size_t n = 0, backed = 0, unsafe = 0, notclean = 0;
    for (const auto& p : m_dv_shown.projects) {
        const auto st = p.repo.status;
        backed   += st == dv::Status::Backed;
        unsafe   += st == dv::Status::NoRemote || st == dv::Status::NotRepo;
        notclean += st == dv::Status::NotClean || st == dv::Status::Unreachable;
        backed   += st == dv::Status::Retired;   // retired = safely on its remote
        for (const auto& b : p.builds) {
            all += b.bytes;
            if (b.offered && b.checked) { bytes += b.bytes; ++n; }
        }
    }
    m_dv_big.set_text(scan::human_bytes(bytes));
    const auto np = m_dv_shown.projects.size();
    m_dv_caption.set_text("of build output chosen, of " + scan::human_bytes(all) + " · " + with_commas(np) +
                          (np == 1 ? " project: " : " projects: ") + with_commas(backed) + " backed up, " +
                          with_commas(notclean) + " not clean, " + with_commas(unsafe) + " not backed up");
    m_dv_go.set_label(n ? "Move " + scan::human_bytes(bytes) + " of build output to the Trash" : "Move to the Trash");
    const bool busy = m_dv_worker.joinable();
    if (m_pf_red)
        m_dv_gate.set_text("Preflight has a red check. Dev projects waits until it clears -- see Preflight.");
    else if (!n)
        m_dv_gate.set_text("Nothing checked.");
    else
        m_dv_gate.set_text("Only build output moves -- sources, docs, backups and fixes/ stay. One step; Undo brings it back.");
    if (m_act_dv_run) m_act_dv_run->set_enabled(n > 0 && !m_pf_red && !busy);
}

void Shell::paint_dev_confirm(const std::string& title, const std::string& sub, const std::string& go,
                              const std::vector<std::string>& keep, const std::vector<std::string>& warn) {  // helper: the preview
    namespace dr = core::door;
    m_dv_cf_title.set_text(title);
    m_dv_cf_sub.set_text(sub);
    m_dv_cf_go.set_label(go);
    while (auto* c = m_dv_cf_list.get_first_child()) m_dv_cf_list.remove(*c);
    const auto tilde = [&](const std::string& p) {
        return p.rfind(m_home + "/", 0) == 0 ? "~/" + p.substr(m_home.size() + 1) : p;
    };
    auto steps = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    steps->add_css_class("card");
    steps->append(*text("What happens, in this order -- one step, Undo takes it all back", "card-title"));
    for (const auto& a : m_dv_plan.actions) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->add_css_class("sweep-row");
        const char* verb = a.verb == dr::Verb::Trash ? "To the Trash" : a.verb == dr::Verb::Write ? "Write"
                         : a.verb == dr::Verb::Clone ? "Clone" : "Move";
        auto v = text(verb, "drift-tag");
        v->add_css_class(a.verb == dr::Verb::Trash ? "pf-amber" : "pf-green");
        row->append(*v);
        auto sz = text(a.bytes ? scan::human_bytes(a.bytes) : "", "detail-size", 1.0f);
        sz->set_width_chars(9);
        row->append(*sz);
        auto words = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
        words->set_hexpand(true);
        auto path = text(tilde(dr::creates(a.verb) ? a.to : a.from), "detail-path");
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        words->append(*path);
        auto why = text(a.reason, "dim");
        why->set_wrap(true);
        words->append(*why);
        row->append(*words);
        if (a.verb == dr::Verb::Write) row->set_tooltip_text(a.content);
        steps->append(*row);
    }
    m_dv_cf_list.append(*steps);
    if (!keep.empty()) {
        std::string k;
        for (const auto& x : keep) k += (k.empty() ? "" : ", ") + x;
        auto l = text("Stays: " + k, "dim");
        l->set_wrap(true);
        m_dv_cf_list.append(*l);
    }
    for (const auto& w : warn) {
        auto l = text(w, "pf-amber");
        l->set_wrap(true);
        m_dv_cf_list.append(*l);
    }
    m_dv_states.set_visible_child("confirm");
}

void Shell::go_to(const std::string& page) {  // helper: select a module's sidebar row
    for (int i = 0; auto* row = m_nav.get_row_at_index(i); ++i)
        if (std::string(row->get_name()) == "nav." + page) { m_nav.select_row(*row); return; }
}

std::string Shell::recents_file() const {  // helper: XDG path (UI-side resolution)
    // The UI resolves the XDG dir (Glib here) and hands the core a plain path --
    // the core stays GTK-free (docs/two-layer-seam.md).
    return (std::filesystem::path(Glib::get_user_data_dir()) / "rouster" / "recent.json")
        .string();
}

void Shell::note_recent(const std::string& path) {  // helper: push + persist + repaint
    if (path.empty()) return;
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    rebuild_recents_menu();
}

void Shell::rebuild_recents_menu() {  // helper: repaint the submenu
    if (!m_recents_menu) return;
    m_recents_menu->remove_all();

    // Display name = folder basename; identical basenames get their parent dir
    // appended so they're distinguishable (Curvz's disambiguation).
    std::vector<std::string> names(m_recents.size());
    for (std::size_t i = 0; i < m_recents.size(); ++i)
        names[i] = std::filesystem::path(m_recents[i]).filename().string();
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j)
            if (names[i] == names[j]) {
                const std::string pi =
                    std::filesystem::path(m_recents[i]).parent_path().filename().string();
                const std::string pj =
                    std::filesystem::path(m_recents[j]).parent_path().filename().string();
                if (!pi.empty()) names[i] += "  (" + pi + ")";
                if (!pj.empty()) names[j] += "  (" + pj + ")";
            }

    for (std::size_t i = 0; i < m_recents.size(); ++i) {
        auto item = Gio::MenuItem::create(names[i], "");
        item->set_action_and_target(
            "recents.open", Glib::Variant<Glib::ustring>::create(m_recents[i]));
        m_recents_menu->append_item(item);
    }

    auto clear_section = Gio::Menu::create();
    clear_section->append("Clear recent", "recents.clear");
    m_recents_menu->append_section("", clear_section);
    if (m_recents_clear) m_recents_clear->set_enabled(!m_recents.empty());
}

}  // namespace rouster
