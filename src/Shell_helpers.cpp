#include "Shell.hpp"
#include "core/Recents.hpp"
#include "Log.hpp"

#include <gtkmm/label.h>
#include <gtkmm/expander.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <giomm/menuitem.h>
#include <glibmm/miscutils.h>
#include <glibmm/variant.h>

#include <chrono>
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
    while (auto* c = m_scan_results.get_first_child()) m_scan_results.remove(*c);
    m_scan_where.set_text("Scanning " + root + " -- read-only, nothing is changed.");
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
    m_tick = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::tick_progress), 150);
}

void Shell::stop_scan() {  // helper: cancel + join (close, dtor)
    if (m_progress) m_progress->cancel = true;
    if (m_worker.joinable()) m_worker.join();
    m_tick.disconnect();
}

bool Shell::tick_progress() {  // helper: repaint the live counters
    if (!m_progress) return false;
    m_scan_status.set_text(with_commas(m_progress->files.load()) + " files · " +
                           scan::human_bytes(m_progress->bytes.load()) +
                           (m_progress->cancel.load() ? " · stopping…" : ""));
    return true;
}

void Shell::set_scanning(bool on) {  // helper: buttons + spinner follow the state
    if (m_act_scan)   m_act_scan->set_enabled(!on);
    if (m_act_pick)   m_act_pick->set_enabled(!on);
    if (m_act_cancel) m_act_cancel->set_enabled(on);
    m_cancel_button.set_visible(on);
    if (on) m_spinner.start(); else m_spinner.stop();
    m_spinner.set_visible(on);
}

void Shell::paint_report() {  // helper: the summary, by kind
    scan::Report r;
    {
        std::lock_guard lk(m_report_mx);
        r = m_report;
    }
    while (auto* c = m_scan_results.get_first_child()) m_scan_results.remove(*c);

    if (!r.refused.empty()) {
        m_scan_where.set_text("Not scanned: " + r.refused);
        m_scan_status.set_text("");
        return;
    }
    m_scan_where.set_text("Scanned " + r.root + (r.cancelled ? " -- stopped early; partial." : ".") +
                          " Nothing was changed.");
    const auto kb = [&](scan::Kind k) { return r.by_kind[static_cast<int>(k)]; };
    std::uint64_t files = 0;
    for (const auto& b : r.by_kind) files += b.files;
    std::string status = scan::human_bytes(r.total_bytes()) + " in " + with_commas(files) + " files";
    if (r.errors) status += " · " + with_commas(r.errors) + " unreadable";
    m_scan_status.set_text(status);

    // The headline: what a later Sweep could give back without a second thought.
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_xalign(0.0f);
    head->set_margin_bottom(6);
    head->set_markup("<span size='large'>Found <b>" + scan::human_bytes(kb(scan::Kind::Regenerable).bytes) +
                     "</b> regenerable</span>    <span alpha='70%'>+ " +
                     scan::human_bytes(kb(scan::Kind::Trash).bytes) + " in the Trash</span>");
    m_scan_results.append(*head);

    constexpr std::size_t kTop = 25;
    const std::string home = r.home;
    for (scan::Kind k : {scan::Kind::Regenerable, scan::Kind::Trash, scan::Kind::State, scan::Kind::Settings,
                         scan::Kind::AppData, scan::Kind::Protected, scan::Kind::Other}) {
        const auto b = kb(k);
        auto ex = Gtk::make_managed<Gtk::Expander>();
        ex->set_use_markup(true);
        ex->set_label("<b>" + esc(scan::kind_name(k)) + "</b>    " + scan::human_bytes(b.bytes) +
                      "  <span alpha='60%'>· " + with_commas(b.files) + " files</span>");
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        body->set_margin_start(22);
        body->set_margin_bottom(6);
        auto why = Gtk::make_managed<Gtk::Label>(scan::kind_treatment(k));
        why->set_xalign(0.0f);
        why->set_wrap(true);
        why->add_css_class("dim-label");
        why->set_margin_bottom(4);
        body->append(*why);

        std::size_t shown = 0, more = 0;
        std::uint64_t more_bytes = 0;
        for (const auto& it : r.items) {
            if (it.kind != k) continue;
            if (shown == kTop) { ++more; more_bytes += it.bytes; continue; }
            ++shown;
            auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            auto sz = Gtk::make_managed<Gtk::Label>(scan::human_bytes(it.bytes));
            sz->set_width_chars(9);
            sz->set_xalign(1.0f);
            auto path = Gtk::make_managed<Gtk::Label>("~/" + it.rel);
            path->set_xalign(0.0f);
            path->set_hexpand(true);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            path->set_tooltip_text(home + "/" + it.rel + "  —  " + with_commas(it.files) + " files");
            line->append(*sz);
            line->append(*path);
            body->append(*line);
        }
        if (more) {
            auto m = Gtk::make_managed<Gtk::Label>("… and " + with_commas(more) + " more (" +
                                                   scan::human_bytes(more_bytes) + ")");
            m->set_xalign(0.0f);
            m->add_css_class("dim-label");
            body->append(*m);
        }
        ex->set_child(*body);
        ex->set_expanded(k == scan::Kind::Regenerable && shown > 0);
        m_scan_results.append(*ex);
    }

    if (!r.skipped.empty()) {
        auto ex = Gtk::make_managed<Gtk::Expander>();
        ex->set_label("Not scanned -- other filesystems (" + with_commas(r.skipped.size()) + ")");
        auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        body->set_margin_start(22);
        for (const auto& s : r.skipped) {
            auto l = Gtk::make_managed<Gtk::Label>("~/" + s);
            l->set_xalign(0.0f);
            body->append(*l);
        }
        ex->set_child(*body);
        m_scan_results.append(*ex);
    }
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
