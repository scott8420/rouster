#include "Shell.hpp"
#include "core/Recents.hpp"
#include "core/Preflight.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/expander.h>
#include <gtkmm/image.h>
#include <gtkmm/flowboxchild.h>
#include <glibmm/main.h>
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

    m_worker = std::thread([this, prog = m_progress, home = m_home, root] {
        const auto t0 = std::chrono::steady_clock::now();
        scan::Report r = scan::scan(home, root, *prog);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        if (auto lg = log::get(log::Area::Scan))
            lg->info("done '{}': {} files, {} dirs, {} in {} ms{}{}", root, prog->files.load(),
                     r.dirs, scan::human_bytes(r.total_bytes()), ms,
                     r.cancelled ? " (cancelled)" : "", r.refused.empty() ? "" : " REFUSED: " + r.refused);
        {
            std::lock_guard lk(m_report_mx);
            m_report = std::move(r);
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
    if (!r.skipped.empty()) {
        where += " · not entered (other filesystems): ";
        for (std::size_t i = 0; i < r.skipped.size() && i < 6; ++i) where += (i ? ", ~/" : "~/") + r.skipped[i];
        if (r.skipped.size() > 6) where += ", …";
    }
    where += ".  Nothing was changed.";
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
    if (k == scan::Kind::Other)
        m_detail.append(*text("Your own files are totalled, not listed -- they are yours to organize.", "dim"));
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
            auto l = text(std::string(mark) + "  " + dr::verb_name(r->verb) + "  " + r->from +
                          (r->error.empty() ? "" : "  -- " + r->error), "detail-path");
            l->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            l->set_tooltip_text(r->from + "\n→ " + r->to);
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
            case pf::Status::Green: icon->set_from_icon_name("emblem-ok-symbolic");      icon->add_css_class("pf-green"); chip = "OK";    break;
            case pf::Status::Amber: icon->set_from_icon_name("dialog-warning-symbolic"); icon->add_css_class("pf-amber"); chip = "NOTE";  break;
            case pf::Status::Red:   icon->set_from_icon_name("dialog-error-symbolic");   icon->add_css_class("pf-red");   chip = "STOP";  break;
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
        card->append(*ch);
        m_pf_rows.append(*card);
        if (auto lg = log::get(log::Area::Preflight))
            lg->info("{} {}: {}", pf::status_name(c.status), c.id, c.detail);
    }
    m_pf_summary.set_text(std::to_string(green) + " of " + std::to_string(checks.size()) + " checks green" +
                          (red ? ", " + std::to_string(red) + " red" : std::string{}) +
                          ". A red check stops Sweep until it clears.");
    m_pf_red = red > 0;
    sweep_gate();
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
